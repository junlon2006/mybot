/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/mybot.h>
#include <mybot/mybot_build_config.h>

#include "mybot_app.h"
#include "mybot_conversation.h"
#include "mybot_device_lifecycle.h"
#include "mybot_https_internal.h"
#include "mybot_key_internal.h"
#include "mybot_kv_store_internal.h"
#include "mybot_media_pipeline.h"
#include "mybot_presenter.h"
#include "mybot_wifi_internal.h"

#include <api/aosl.h>
#include <api/aosl_atomic.h>
#include <api/aosl_log.h>
#include <api/aosl_mpq.h>
#include <api/aosl_mpq_timer.h>

#include <string.h>

#define STATE_TICK_MS 100
#define APP_MPQ_STACK_SIZE 16384
#define VOLUME_KEY_STEP 10

struct mybot_runtime {
    aosl_atomic_t running;
    aosl_atomic_t state;
    aosl_atomic_t aosl_ref_held;
    mybot_config_t config;

    mybot_key_t key;
    mybot_kv_store_t kv_store;
    mybot_wifi_t wifi;
    mybot_presenter_t presenter;
    mybot_media_pipeline_t media;
    mybot_conversation_t conversation;
    mybot_device_lifecycle_t lifecycle;

    aosl_mpq_t startup_mpq;
    aosl_mpq_t state_mpq;
    aosl_timer_t state_timer;
};

static mybot_runtime_t s_default_runtime;

static mybot_state_t runtime_get_state(const mybot_runtime_t *runtime) {
    return (mybot_state_t)aosl_atomic_read(&runtime->state);
}

static void runtime_request_exit(mybot_runtime_t *runtime) {
    aosl_atomic_set(&runtime->running, false);
    mybot_media_pipeline_request_stop(&runtime->media);
}

static void sync_wake_words(mybot_runtime_t *runtime) {
#if MYBOT_WAKE_WORDS
    bool enabled =
        runtime_get_state(runtime) == MYBOT_STATE_READY &&
        mybot_device_lifecycle_get_state(&runtime->lifecycle) == MYBOT_DEVICE_STATE_RUNTIME;
    mybot_media_pipeline_set_wake_words_enabled(&runtime->media, enabled);
#else
    (void)runtime;
#endif
}

static int media_send_audio(const void *data, size_t len, void *user_data) {
    mybot_runtime_t *runtime = user_data;
    return mybot_conversation_send_audio(&runtime->conversation, data, len);
}

static void media_on_wake_word(const char *wake_word, void *user_data) {
    mybot_runtime_t *runtime = user_data;
    AOSL_LOG_NTC("[WAKE WORDS] detected: %s", wake_word ? wake_word : "<unspecified>");
    mybot_app_start_conversation(runtime);
}

static void conversation_on_remote_audio(uint32_t uid, const void *data, size_t len,
                                         void *user_data) {
    (void)uid;
    mybot_runtime_t *runtime = user_data;
    mybot_media_pipeline_push_remote_audio(&runtime->media, data, len);
}

static void conversation_on_state_changed(mybot_rtc_state_t state, void *user_data) {
    mybot_runtime_t *runtime = user_data;
    bool connected = state == MYBOT_RTC_STATE_CONNECTED;
    mybot_media_pipeline_set_rtc_connected(&runtime->media, connected);
    AOSL_LOG_NTC("rtc -> %s", connected ? "connected" : "disconnected");

    if (state == MYBOT_RTC_STATE_DISCONNECTED || state == MYBOT_RTC_STATE_ERROR) {
        mybot_device_lifecycle_notify_conversation_ended(&runtime->lifecycle);
    }
}

static void conversation_on_token_will_expire(void *user_data) {
    mybot_runtime_t *runtime = user_data;
    if (aosl_atomic_read(&runtime->running)) {
        mybot_device_lifecycle_request_rtc_token_renewal(&runtime->lifecycle);
    }
}

static void dev_on_pair_code(const char *code, void *user_data) {
    mybot_runtime_t *runtime = user_data;
    mybot_state_t state = runtime_get_state(runtime);
    if (state == MYBOT_STATE_STOPPING || state == MYBOT_STATE_FAILED ||
        state == MYBOT_STATE_WIFI_DISCONNECTED) {
        return;
    }

    AOSL_LOG_NTC("pair code: %s", code);
    mybot_presenter_show_pair_code(&runtime->presenter, code);
    mybot_media_pipeline_play_pair_code(&runtime->media, code);
}

static int dev_on_rtc_token_renewed(const char *token, void *user_data) {
    mybot_runtime_t *runtime = user_data;
    if (!aosl_atomic_read(&runtime->running)) {
        return -1;
    }
    return mybot_conversation_renew_token(&runtime->conversation, token);
}

static void dev_on_conversation_start(const mybot_conversation_params_t *params, void *user_data) {
    mybot_runtime_t *runtime = user_data;
    if (!aosl_atomic_read(&runtime->running)) {
        mybot_device_lifecycle_notify_conversation_ended(&runtime->lifecycle);
        return;
    }

    mybot_conversation_callbacks_t callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.on_remote_audio = conversation_on_remote_audio;
    callbacks.on_state_changed = conversation_on_state_changed;
    callbacks.on_token_will_expire = conversation_on_token_will_expire;
    callbacks.user_data = runtime;

    if (mybot_conversation_start(&runtime->conversation, params, &callbacks) < 0) {
        mybot_device_lifecycle_notify_conversation_ended(&runtime->lifecycle);
    }
}

static void dev_on_conversation_stop(void *user_data) {
    mybot_runtime_t *runtime = user_data;
    mybot_media_pipeline_set_rtc_connected(&runtime->media, false);
    if (mybot_conversation_stop(&runtime->conversation) < 0) {
        AOSL_LOG_ERR("failed to leave RTC conversation");
    }
}

static void dev_on_state_changed(mybot_device_state_t state, void *user_data) {
    mybot_runtime_t *runtime = user_data;
    mybot_state_t expected = state == MYBOT_DEVICE_STATE_IN_CONVERSATION
                                 ? MYBOT_STATE_READY
                                 : MYBOT_STATE_IN_CONVERSATION;
    mybot_state_t next = state == MYBOT_DEVICE_STATE_IN_CONVERSATION ? MYBOT_STATE_IN_CONVERSATION
                                                                     : MYBOT_STATE_READY;
    mybot_state_t previous = (mybot_state_t)aosl_atomic_cmpxchg(&runtime->state, expected, next);
    if (previous != expected && previous != next) {
        return;
    }

    if (state != MYBOT_DEVICE_STATE_AWAITING_CLAIM) {
        mybot_media_pipeline_stop_announcement(&runtime->media);
    }
    sync_wake_words(runtime);
    mybot_presenter_render_device_state(&runtime->presenter, state, runtime_get_state(runtime));
}

static void on_key_event(mybot_key_event_t event, void *user_data) {
    mybot_runtime_t *runtime = user_data;
    switch (event) {
    case MYBOT_KEY_EVENT_CONVERSATION_START:
        mybot_app_start_conversation(runtime);
        break;
    case MYBOT_KEY_EVENT_CONVERSATION_STOP:
        mybot_app_stop_conversation(runtime);
        break;
    case MYBOT_KEY_EVENT_PAIR:
        mybot_app_pair(runtime);
        break;
    case MYBOT_KEY_EVENT_VOLUME_UP:
        mybot_media_pipeline_adjust_volume(&runtime->media, VOLUME_KEY_STEP);
        break;
    case MYBOT_KEY_EVENT_VOLUME_DOWN:
        mybot_media_pipeline_adjust_volume(&runtime->media, -VOLUME_KEY_STEP);
        break;
    case MYBOT_KEY_EVENT_EXIT:
        runtime_request_exit(runtime);
        break;
    }
}

static void state_tick_timer(aosl_timer_t id, const aosl_ts_t *now, uintptr_t argc,
                             uintptr_t argv[]) {
    (void)id;
    (void)now;
    if (argc != 1) {
        return;
    }

    mybot_runtime_t *runtime = (mybot_runtime_t *)argv[0];
    if (aosl_atomic_read(&runtime->running)) {
        mybot_device_lifecycle_tick(&runtime->lifecycle);
    }
}

static int state_worker_init(void *arg) {
    mybot_runtime_t *runtime = arg;
    runtime->state_timer =
        aosl_mpq_set_timer(STATE_TICK_MS, state_tick_timer, NULL, 1, (uintptr_t)runtime);
    return aosl_mpq_timer_invalid(runtime->state_timer) ? -1 : 0;
}

static void state_worker_fini(void *arg) {
    mybot_runtime_t *runtime = arg;
    if (!aosl_mpq_timer_invalid(runtime->state_timer)) {
        aosl_mpq_kill_timer(runtime->state_timer);
        runtime->state_timer = AOSL_MPQ_TIMER_INVALID;
    }
    mybot_device_lifecycle_shutdown(&runtime->lifecycle);
}

static void cleanup_services(mybot_runtime_t *runtime) {
    mybot_key_deinit(&runtime->key);

    if (!aosl_mpq_invalid(runtime->state_mpq)) {
        aosl_mpq_destroy_wait(runtime->state_mpq);
        runtime->state_mpq = AOSL_MPQ_INVALID;
    }

    mybot_media_pipeline_stop(&runtime->media);
    mybot_kv_store_deinit(&runtime->kv_store);
}

static int start_services(mybot_runtime_t *runtime) {
    if (mybot_kv_store_init(&runtime->kv_store) < 0 ||
        mybot_key_init(&runtime->key, on_key_event, runtime) < 0) {
        goto fail;
    }

    mybot_media_pipeline_callbacks_t media_cbs;
    memset(&media_cbs, 0, sizeof(media_cbs));
    media_cbs.send_audio = media_send_audio;
    media_cbs.on_wake_word = media_on_wake_word;
    media_cbs.user_data = runtime;
    if (mybot_media_pipeline_start(&runtime->media, &media_cbs) < 0) {
        goto fail;
    }

    mybot_device_lifecycle_callbacks_t lifecycle_cbs;
    memset(&lifecycle_cbs, 0, sizeof(lifecycle_cbs));
    lifecycle_cbs.on_pair_code = dev_on_pair_code;
    lifecycle_cbs.on_conversation_start = dev_on_conversation_start;
    lifecycle_cbs.on_conversation_stop = dev_on_conversation_stop;
    lifecycle_cbs.on_rtc_token_renewed = dev_on_rtc_token_renewed;
    lifecycle_cbs.on_state_changed = dev_on_state_changed;
    lifecycle_cbs.user_data = runtime;
    if (mybot_device_lifecycle_init(&runtime->lifecycle, &runtime->kv_store,
                                    runtime->config.server_base, runtime->config.device_id,
                                    runtime->config.firmware_ver, runtime->config.hw_model,
                                    &lifecycle_cbs) < 0) {
        goto fail;
    }

    runtime->state_mpq =
        aosl_mpq_create(AOSL_THRD_PRI_NORMAL, APP_MPQ_STACK_SIZE, 1000, "state_mpq",
                        state_worker_init, state_worker_fini, runtime);
    if (aosl_mpq_invalid(runtime->state_mpq)) {
        goto fail;
    }
    return 0;

fail:
    cleanup_services(runtime);
    mybot_media_pipeline_destroy(&runtime->media);
    return -1;
}

static void handle_wifi_event(const aosl_ts_t *queued_ts, aosl_refobj_t robj, uintptr_t argc,
                              uintptr_t argv[]) {
    (void)queued_ts;
    (void)robj;
    if (argc != 2) {
        return;
    }

    mybot_runtime_t *runtime = (mybot_runtime_t *)argv[0];
    mybot_wifi_event_t event = (mybot_wifi_event_t)argv[1];
    mybot_state_t state = runtime_get_state(runtime);
    if (state == MYBOT_STATE_STOPPING || state == MYBOT_STATE_FAILED) {
        return;
    }

    if (event == MYBOT_WIFI_EVENT_FAILED && state == MYBOT_STATE_WIFI_PROVISIONING) {
        if (aosl_atomic_cmpxchg(&runtime->state, MYBOT_STATE_WIFI_PROVISIONING,
                                MYBOT_STATE_FAILED) == MYBOT_STATE_WIFI_PROVISIONING) {
            mybot_presenter_show_screen(&runtime->presenter, MYBOT_LCD_SCREEN_FAILED);
            runtime_request_exit(runtime);
        }
        return;
    }

    if (event == MYBOT_WIFI_EVENT_STA_DISCONNECTED ||
        (event == MYBOT_WIFI_EVENT_FAILED && state != MYBOT_STATE_WIFI_PROVISIONING)) {
        if (state != MYBOT_STATE_WIFI_DISCONNECTED) {
            mybot_device_lifecycle_set_network_available(&runtime->lifecycle, false);
            aosl_atomic_set(&runtime->state, MYBOT_STATE_WIFI_DISCONNECTED);
            sync_wake_words(runtime);
        }
        mybot_presenter_show_screen(&runtime->presenter, MYBOT_LCD_SCREEN_WIFI_DISCONNECTED);
        return;
    }

    if (event == MYBOT_WIFI_EVENT_STA_CONNECTED && state == MYBOT_STATE_WIFI_DISCONNECTED) {
        mybot_device_lifecycle_set_network_available(&runtime->lifecycle, true);
        aosl_atomic_set(&runtime->state, MYBOT_STATE_READY);
        sync_wake_words(runtime);
        mybot_presenter_render_device_state(&runtime->presenter,
                                            mybot_device_lifecycle_get_state(&runtime->lifecycle),
                                            runtime_get_state(runtime));
        return;
    }

    if (event != MYBOT_WIFI_EVENT_STA_CONNECTED ||
        aosl_atomic_cmpxchg(&runtime->state, MYBOT_STATE_WIFI_PROVISIONING,
                            MYBOT_STATE_STARTING_SERVICES) != MYBOT_STATE_WIFI_PROVISIONING) {
        return;
    }

    mybot_presenter_show_screen(&runtime->presenter, MYBOT_LCD_SCREEN_STARTING_SERVICES);
    if (start_services(runtime) < 0) {
        if (runtime_get_state(runtime) != MYBOT_STATE_STOPPING) {
            aosl_atomic_set(&runtime->state, MYBOT_STATE_FAILED);
            mybot_presenter_show_screen(&runtime->presenter, MYBOT_LCD_SCREEN_FAILED);
            runtime_request_exit(runtime);
        }
        return;
    }

    if (aosl_atomic_cmpxchg(&runtime->state, MYBOT_STATE_STARTING_SERVICES, MYBOT_STATE_READY) ==
        MYBOT_STATE_STARTING_SERVICES) {
        sync_wake_words(runtime);
        mybot_presenter_render_device_state(&runtime->presenter,
                                            mybot_device_lifecycle_get_state(&runtime->lifecycle),
                                            runtime_get_state(runtime));
    }
}

static void on_wifi_event(mybot_wifi_event_t event, void *user_data) {
    mybot_runtime_t *runtime = user_data;
    mybot_state_t state = runtime_get_state(runtime);
    if (state == MYBOT_STATE_STOPPING || state == MYBOT_STATE_FAILED ||
        state == MYBOT_STATE_STOPPED) {
        return;
    }

    if (aosl_mpq_queue(runtime->startup_mpq, AOSL_MPQ_INVALID, AOSL_REF_INVALID,
                       "handle_wifi_event", handle_wifi_event, 2, (uintptr_t)runtime,
                       (uintptr_t)event) < 0 &&
        aosl_atomic_cmpxchg(&runtime->state, state, MYBOT_STATE_FAILED) == state) {
        mybot_presenter_show_screen(&runtime->presenter, MYBOT_LCD_SCREEN_FAILED);
        runtime_request_exit(runtime);
    }
}

static bool config_is_valid(const mybot_config_t *cfg) {
    return cfg && memchr(cfg->server_base, '\0', sizeof(cfg->server_base)) &&
           memchr(cfg->device_id, '\0', sizeof(cfg->device_id)) &&
           memchr(cfg->firmware_ver, '\0', sizeof(cfg->firmware_ver)) &&
           memchr(cfg->hw_model, '\0', sizeof(cfg->hw_model)) && cfg->server_base[0] &&
           cfg->device_id[0];
}

static bool server_scheme_is_supported(const char *server_base) {
    bool use_https = strncmp(server_base, "https://", 8) == 0;
    bool use_http = strncmp(server_base, "http://", 7) == 0;
#if MYBOT_ENABLE_HTTPS
    if (use_https && !mybot_https_is_registered()) {
        return false;
    }
#else
    if (use_https) {
        return false;
    }
#endif
#if !MYBOT_ALLOW_INSECURE_HTTP
    if (use_http) {
        return false;
    }
#endif
    return use_https || use_http;
}

int mybot_start(const mybot_config_t *cfg) {
    mybot_runtime_t *runtime = &s_default_runtime;
    if (aosl_atomic_read(&runtime->aosl_ref_held) || !config_is_valid(cfg) ||
        !server_scheme_is_supported(cfg->server_base)) {
        return -1;
    }

    memset(runtime, 0, sizeof(*runtime));
    memcpy(&runtime->config, cfg, sizeof(runtime->config));
    runtime->startup_mpq = AOSL_MPQ_INVALID;
    runtime->state_mpq = AOSL_MPQ_INVALID;
    runtime->state_timer = AOSL_MPQ_TIMER_INVALID;
    aosl_atomic_set(&runtime->running, true);
    aosl_atomic_set(&runtime->state, MYBOT_STATE_STOPPED);

    aosl_ctor();
    aosl_atomic_set(&runtime->aosl_ref_held, true);

    if (mybot_presenter_init(&runtime->presenter) < 0) {
        goto fail;
    }
    mybot_presenter_show_screen(&runtime->presenter, MYBOT_LCD_SCREEN_STARTING);

    runtime->startup_mpq = aosl_mpq_create(AOSL_THRD_PRI_NORMAL, APP_MPQ_STACK_SIZE, 32,
                                           "startup_mpq", NULL, NULL, NULL);
    if (aosl_mpq_invalid(runtime->startup_mpq)) {
        goto fail;
    }

    aosl_atomic_set(&runtime->state, MYBOT_STATE_WIFI_PROVISIONING);
    mybot_presenter_show_screen(&runtime->presenter, MYBOT_LCD_SCREEN_WIFI_PROVISIONING);
    if (mybot_wifi_init(&runtime->wifi, runtime->config.device_id, on_wifi_event, runtime) < 0) {
        goto fail;
    }
    return 0;

fail:
    aosl_atomic_set(&runtime->state, MYBOT_STATE_FAILED);
    mybot_stop();
    return -1;
}

bool mybot_is_running(void) {
    return aosl_atomic_read(&s_default_runtime.running) != 0;
}

mybot_state_t mybot_get_state(void) {
    return runtime_get_state(&s_default_runtime);
}

void mybot_request_exit(void) {
    runtime_request_exit(&s_default_runtime);
}

void mybot_app_start_conversation(mybot_runtime_t *runtime) {
    if (runtime_get_state(runtime) == MYBOT_STATE_READY) {
        mybot_device_lifecycle_request_start(&runtime->lifecycle);
    }
}

void mybot_app_stop_conversation(mybot_runtime_t *runtime) {
    if (runtime_get_state(runtime) == MYBOT_STATE_IN_CONVERSATION) {
        mybot_device_lifecycle_request_stop(&runtime->lifecycle);
    }
}

void mybot_app_pair(mybot_runtime_t *runtime) {
    mybot_state_t state = runtime_get_state(runtime);
    if (state == MYBOT_STATE_READY || state == MYBOT_STATE_IN_CONVERSATION) {
        mybot_device_lifecycle_request_pair(&runtime->lifecycle);
    }
}

void mybot_stop(void) {
    mybot_runtime_t *runtime = &s_default_runtime;
    if (!aosl_atomic_read(&runtime->aosl_ref_held)) {
        return;
    }

    mybot_state_t previous = runtime_get_state(runtime);
    aosl_atomic_set(&runtime->state, MYBOT_STATE_STOPPING);
    runtime_request_exit(runtime);
    if (previous != MYBOT_STATE_FAILED) {
        mybot_presenter_show_screen(&runtime->presenter, MYBOT_LCD_SCREEN_STOPPING);
    }

    if (!aosl_mpq_invalid(runtime->startup_mpq)) {
        aosl_mpq_destroy_wait(runtime->startup_mpq);
        runtime->startup_mpq = AOSL_MPQ_INVALID;
    }

    cleanup_services(runtime);
    mybot_wifi_deinit(&runtime->wifi);

    mybot_presenter_show_screen(&runtime->presenter, previous == MYBOT_STATE_FAILED
                                                         ? MYBOT_LCD_SCREEN_FAILED
                                                         : MYBOT_LCD_SCREEN_STOPPING);
    mybot_presenter_deinit(&runtime->presenter);

    mybot_conversation_fini(&runtime->conversation);
    mybot_media_pipeline_destroy(&runtime->media);

    aosl_atomic_set(&runtime->state, MYBOT_STATE_STOPPED);
    aosl_dtor();
    aosl_atomic_set(&runtime->aosl_ref_held, false);
}
