/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/mybot.h>
#include <mybot/mybot_build_config.h>
#include <mybot/platform/mybot_platform.h>

#include "mybot_agora_rtc.h"
#include "mybot_device_lifecycle.h"
#include "mybot_key_internal.h"
#include "mybot_kv_store_internal.h"
#include "mybot_media_pipeline.h"
#include "mybot_presenter.h"
#include "mybot_platform_registry.h"
#include "mybot_state_model.h"
#include "mybot_wifi_internal.h"

#include <api/aosl.h>
#include <api/aosl_atomic.h>
#include <api/aosl_log.h>
#include <api/aosl_mpq.h>
#include <api/aosl_mpq_timer.h>
#include <api/aosl_time.h>

#include <string.h>

#define STATE_TICK_MS 100
#define CONTROL_MPQ_STACK_SIZE 16384
#define VOLUME_KEY_STEP 10

typedef struct {
    aosl_atomic_t running;
    mybot_state_model_t state_model;
    aosl_atomic_t aosl_ref_held;
    mybot_config_t config;

    mybot_key_t key;
    mybot_kv_store_t kv_store;
    mybot_wifi_t wifi;
    mybot_presenter_t presenter;
    mybot_media_pipeline_t media;
    mybot_device_lifecycle_t lifecycle;

    /* Sole owner of application state transitions and control resources. */
    aosl_mpq_t control_mpq;
    aosl_timer_t control_timer;
    bool lifecycle_initialized;
    int control_start_result;
} mybot_runtime_t;

static mybot_runtime_t s_default_runtime;
static aosl_atomic_t s_lifecycle_gate;
static aosl_atomic_t s_exit_generation;
static aosl_atomic_t s_run_exit_generation;

/* This gate must work before aosl_ctor(), so it intentionally uses only the
 * HAL-backed atomic primitive, matching AOSL's own lifecycle gate. */
static void lifecycle_lock(void) {
    while (aosl_atomic_cmpxchg(&s_lifecycle_gate, 0, 1) != 0) {
        aosl_msleep(5);
    }
}

static void lifecycle_unlock(void) {
    aosl_atomic_set(&s_lifecycle_gate, 0);
}

static mybot_state_t runtime_get_state(const mybot_runtime_t *runtime) {
    return mybot_state_model_get_view(&runtime->state_model).app_state;
}

static bool runtime_is_running(const mybot_runtime_t *runtime) {
    return aosl_atomic_read(&runtime->running) != 0 &&
           aosl_atomic_read(&s_exit_generation) == aosl_atomic_read(&s_run_exit_generation);
}

static void runtime_publish_exit(mybot_runtime_t *runtime) {
    aosl_atomic_inc(&s_exit_generation);
    aosl_atomic_set(&runtime->running, false);
}

static void control_start_conversation(mybot_runtime_t *runtime) {
    if (runtime_get_state(runtime) == MYBOT_STATE_READY) {
        mybot_device_lifecycle_request_start(&runtime->lifecycle);
    }
}

static void control_stop_conversation(mybot_runtime_t *runtime) {
    if (runtime_get_state(runtime) == MYBOT_STATE_IN_CONVERSATION) {
        mybot_device_lifecycle_request_stop(&runtime->lifecycle);
    }
}

static void control_pair(mybot_runtime_t *runtime) {
    mybot_state_t state = runtime_get_state(runtime);
    if (state == MYBOT_STATE_READY || state == MYBOT_STATE_IN_CONVERSATION) {
        mybot_device_lifecycle_request_pair(&runtime->lifecycle);
    }
}

static void handle_start_conversation(const aosl_ts_t *queued_ts, aosl_refobj_t robj,
                                      uintptr_t argc, uintptr_t argv[]) {
    (void)queued_ts;
    (void)robj;
    if (argc == 1) {
        mybot_runtime_t *runtime = (mybot_runtime_t *)argv[0];
        if (runtime_is_running(runtime)) {
            control_start_conversation(runtime);
        }
    }
}

static void fail_control_queue(mybot_runtime_t *runtime, const char *event) {
    AOSL_LOG_ERR("failed to queue control event: %s", event);
    runtime_publish_exit(runtime);
}

static void sync_wake_words(mybot_runtime_t *runtime) {
#if MYBOT_WAKE_WORDS
    mybot_state_view_t state = mybot_state_model_get_view(&runtime->state_model);
    bool enabled =
        state.app_state == MYBOT_STATE_READY && state.device_state == MYBOT_DEVICE_STATE_RUNTIME;
    mybot_media_pipeline_set_wake_words_enabled(&runtime->media, enabled);
#else
    (void)runtime;
#endif
}

static int media_send_audio(const void *data, size_t len, void *user_data) {
    (void)user_data;
    return mybot_agora_rtc_send_audio(data, len);
}

static void media_on_wake_word(const char *wake_word, void *user_data) {
    mybot_runtime_t *runtime = user_data;
    AOSL_LOG_NTC("[WAKE WORDS] detected: %s", wake_word ? wake_word : "<unspecified>");
    if (!runtime_is_running(runtime) || runtime_get_state(runtime) != MYBOT_STATE_READY) {
        return;
    }
    if (aosl_mpq_queue(runtime->control_mpq, AOSL_MPQ_INVALID, AOSL_REF_INVALID,
                       "handle_start_conversation", handle_start_conversation, 1,
                       (uintptr_t)runtime) < 0) {
        fail_control_queue(runtime, "wake-word conversation start");
    }
}

static void rtc_on_remote_audio(uint32_t uid, const void *data, size_t len, void *user_data) {
    (void)uid;
    mybot_runtime_t *runtime = user_data;
    mybot_media_pipeline_push_remote_audio(&runtime->media, data, len);
}

static void rtc_on_state_changed(mybot_rtc_state_t state, void *user_data) {
    mybot_runtime_t *runtime = user_data;
    bool connected = state == MYBOT_RTC_STATE_CONNECTED;
    mybot_media_pipeline_set_rtc_connected(&runtime->media, connected);
    AOSL_LOG_NTC("rtc -> %s", connected ? "connected" : "disconnected");

    if (state == MYBOT_RTC_STATE_DISCONNECTED || state == MYBOT_RTC_STATE_ERROR) {
        mybot_device_lifecycle_notify_conversation_ended(&runtime->lifecycle);
    }
}

static void rtc_on_token_will_expire(void *user_data) {
    mybot_runtime_t *runtime = user_data;
    if (runtime_is_running(runtime)) {
        mybot_device_lifecycle_request_rtc_token_renewal(&runtime->lifecycle);
    }
}

static void dev_on_pair_code(const char *code, void *user_data) {
    mybot_runtime_t *runtime = user_data;
    if (!runtime_is_running(runtime)) {
        return;
    }
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
    if (!runtime_is_running(runtime)) {
        return -1;
    }
    return mybot_agora_rtc_renew_token(token);
}

static void dev_on_conversation_start(const mybot_conversation_params_t *params, void *user_data) {
    mybot_runtime_t *runtime = user_data;
    if (!runtime_is_running(runtime)) {
        mybot_device_lifecycle_notify_conversation_ended(&runtime->lifecycle);
        return;
    }

    mybot_agora_rtc_callbacks_t callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.on_remote_audio = rtc_on_remote_audio;
    callbacks.on_state_changed = rtc_on_state_changed;
    callbacks.on_token_will_expire = rtc_on_token_will_expire;
    callbacks.user_data = runtime;

    if (mybot_agora_rtc_init(params->rtc_app_id, &callbacks) < 0) {
        AOSL_LOG_ERR("failed to initialize Agora RTC");
        mybot_device_lifecycle_notify_conversation_ended(&runtime->lifecycle);
        return;
    }

    AOSL_LOG_NTC("joining RTC channel=%s uid=%s", params->rtc_channel, params->rtc_uid);
    if (mybot_agora_rtc_join(params->rtc_channel, params->rtc_token, params->rtc_uid) < 0) {
        AOSL_LOG_ERR("failed to join Agora RTC channel");
        mybot_device_lifecycle_notify_conversation_ended(&runtime->lifecycle);
        return;
    }
    AOSL_LOG_NTC("RTC join requested");
}

static void dev_on_conversation_stop(void *user_data) {
    mybot_runtime_t *runtime = user_data;
    mybot_media_pipeline_set_rtc_connected(&runtime->media, false);
    if (mybot_agora_rtc_leave() < 0) {
        AOSL_LOG_ERR("failed to leave RTC conversation");
    }
}

static void dev_on_state_changed(mybot_device_state_t state, void *user_data) {
    mybot_runtime_t *runtime = user_data;
    if (!runtime_is_running(runtime)) {
        return;
    }
    (void)mybot_state_model_set_device_state(&runtime->state_model, state);

    if (state != MYBOT_DEVICE_STATE_AWAITING_CLAIM) {
        mybot_media_pipeline_stop_announcement(&runtime->media);
    }
    sync_wake_words(runtime);
    mybot_presenter_render_state(&runtime->presenter, &runtime->state_model);
}

static void handle_key_event(const aosl_ts_t *queued_ts, aosl_refobj_t robj, uintptr_t argc,
                             uintptr_t argv[]) {
    (void)queued_ts;
    (void)robj;
    if (argc != 2) {
        return;
    }

    mybot_runtime_t *runtime = (mybot_runtime_t *)argv[0];
    if (!runtime_is_running(runtime)) {
        return;
    }
    mybot_key_event_t event = (mybot_key_event_t)argv[1];
    switch (event) {
    case MYBOT_KEY_EVENT_CONVERSATION_START:
        control_start_conversation(runtime);
        break;
    case MYBOT_KEY_EVENT_CONVERSATION_STOP:
        control_stop_conversation(runtime);
        break;
    case MYBOT_KEY_EVENT_PAIR:
        control_pair(runtime);
        break;
    case MYBOT_KEY_EVENT_VOLUME_UP:
        mybot_media_pipeline_adjust_volume(&runtime->media, VOLUME_KEY_STEP);
        break;
    case MYBOT_KEY_EVENT_VOLUME_DOWN:
        mybot_media_pipeline_adjust_volume(&runtime->media, -VOLUME_KEY_STEP);
        break;
    case MYBOT_KEY_EVENT_EXIT:
        break;
    }
}

static void on_key_event(mybot_key_event_t event, void *user_data) {
    mybot_runtime_t *runtime = user_data;
    if (event == MYBOT_KEY_EVENT_EXIT) {
        runtime_publish_exit(runtime);
        return;
    }
    if (!runtime_is_running(runtime)) {
        return;
    }

    mybot_state_t state = runtime_get_state(runtime);
    if ((event == MYBOT_KEY_EVENT_CONVERSATION_START && state != MYBOT_STATE_READY) ||
        (event == MYBOT_KEY_EVENT_CONVERSATION_STOP && state != MYBOT_STATE_IN_CONVERSATION) ||
        (event == MYBOT_KEY_EVENT_PAIR && state != MYBOT_STATE_READY &&
         state != MYBOT_STATE_IN_CONVERSATION)) {
        return;
    }

    if (aosl_mpq_queue(runtime->control_mpq, AOSL_MPQ_INVALID, AOSL_REF_INVALID, "handle_key_event",
                       handle_key_event, 2, (uintptr_t)runtime, (uintptr_t)event) < 0) {
        fail_control_queue(runtime, "key");
    }
}

static void control_tick_timer(aosl_timer_t id, const aosl_ts_t *now, uintptr_t argc,
                               uintptr_t argv[]) {
    (void)id;
    (void)now;
    if (argc != 1) {
        return;
    }

    mybot_runtime_t *runtime = (mybot_runtime_t *)argv[0];
    if (runtime_is_running(runtime)) {
        mybot_device_lifecycle_tick(&runtime->lifecycle);
    }
}

static void cleanup_services(mybot_runtime_t *runtime) {
    mybot_key_deinit(&runtime->key);

    if (!aosl_mpq_timer_invalid(runtime->control_timer)) {
        if (aosl_mpq_kill_timer(runtime->control_timer) < 0) {
            AOSL_LOG_ERR("failed to stop device lifecycle timer");
        }
        runtime->control_timer = AOSL_MPQ_TIMER_INVALID;
    }
    if (runtime->lifecycle_initialized) {
        mybot_device_lifecycle_shutdown(&runtime->lifecycle);
        runtime->lifecycle_initialized = false;
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
    runtime->lifecycle_initialized = true;

    runtime->control_timer =
        aosl_mpq_set_timer(STATE_TICK_MS, control_tick_timer, NULL, 1, (uintptr_t)runtime);
    if (aosl_mpq_timer_invalid(runtime->control_timer)) {
        AOSL_LOG_ERR("failed to create device lifecycle timer");
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
    if (argc != 3) {
        return;
    }

    mybot_runtime_t *runtime = (mybot_runtime_t *)argv[0];
    if (!runtime_is_running(runtime)) {
        return;
    }
    mybot_wifi_event_t event = (mybot_wifi_event_t)argv[1];
    bool network_published = argv[2] != 0;
    mybot_state_t state = runtime_get_state(runtime);
    if (state == MYBOT_STATE_STOPPING || state == MYBOT_STATE_FAILED) {
        return;
    }

    if (runtime->lifecycle_initialized && !network_published) {
        if (event == MYBOT_WIFI_EVENT_STA_CONNECTED) {
            mybot_device_lifecycle_set_network_available(&runtime->lifecycle, true);
        } else if (event == MYBOT_WIFI_EVENT_STA_DISCONNECTED || event == MYBOT_WIFI_EVENT_FAILED) {
            mybot_device_lifecycle_set_network_available(&runtime->lifecycle, false);
        }
    }

    if (event == MYBOT_WIFI_EVENT_FAILED && state == MYBOT_STATE_WIFI_PROVISIONING) {
        if (mybot_state_model_fail(&runtime->state_model)) {
            mybot_presenter_show_screen(&runtime->presenter, MYBOT_LCD_SCREEN_FAILED);
            runtime_publish_exit(runtime);
        }
        return;
    }

    if (event == MYBOT_WIFI_EVENT_STA_DISCONNECTED ||
        (event == MYBOT_WIFI_EVENT_FAILED && state != MYBOT_STATE_WIFI_PROVISIONING)) {
        if (state != MYBOT_STATE_WIFI_DISCONNECTED &&
            mybot_state_model_network_lost(&runtime->state_model)) {
            sync_wake_words(runtime);
        }
        if (runtime_get_state(runtime) == MYBOT_STATE_WIFI_DISCONNECTED) {
            mybot_presenter_show_screen(&runtime->presenter, MYBOT_LCD_SCREEN_WIFI_DISCONNECTED);
        }
        return;
    }

    if (event == MYBOT_WIFI_EVENT_STA_CONNECTED && state == MYBOT_STATE_WIFI_DISCONNECTED) {
        if (mybot_state_model_network_restored(&runtime->state_model)) {
            sync_wake_words(runtime);
            mybot_presenter_render_state(&runtime->presenter, &runtime->state_model);
        }
        return;
    }

    if (event != MYBOT_WIFI_EVENT_STA_CONNECTED ||
        !mybot_state_model_begin_services(&runtime->state_model)) {
        return;
    }

    mybot_presenter_show_screen(&runtime->presenter, MYBOT_LCD_SCREEN_STARTING_SERVICES);
    if (start_services(runtime) < 0) {
        if (mybot_state_model_fail(&runtime->state_model)) {
            mybot_presenter_show_screen(&runtime->presenter, MYBOT_LCD_SCREEN_FAILED);
            runtime_publish_exit(runtime);
        }
        return;
    }

    if (mybot_state_model_services_ready(&runtime->state_model)) {
        AOSL_LOG_NTC("application services ready");
        sync_wake_words(runtime);
        mybot_presenter_render_state(&runtime->presenter, &runtime->state_model);
    }
}

static void on_wifi_event(mybot_wifi_event_t event, void *user_data) {
    mybot_runtime_t *runtime = user_data;
    if (!runtime_is_running(runtime)) {
        return;
    }
    mybot_state_t state = runtime_get_state(runtime);
    if (state == MYBOT_STATE_STOPPING || state == MYBOT_STATE_FAILED ||
        state == MYBOT_STATE_STOPPED) {
        return;
    }

    bool network_published = false;
    if (state == MYBOT_STATE_READY || state == MYBOT_STATE_IN_CONVERSATION ||
        state == MYBOT_STATE_WIFI_DISCONNECTED) {
        if (event == MYBOT_WIFI_EVENT_STA_CONNECTED) {
            mybot_device_lifecycle_set_network_available(&runtime->lifecycle, true);
            network_published = true;
        } else if (event == MYBOT_WIFI_EVENT_STA_DISCONNECTED || event == MYBOT_WIFI_EVENT_FAILED) {
            mybot_device_lifecycle_set_network_available(&runtime->lifecycle, false);
            network_published = true;
        }
    }

    if (aosl_mpq_queue(runtime->control_mpq, AOSL_MPQ_INVALID, AOSL_REF_INVALID,
                       "handle_wifi_event", handle_wifi_event, 3, (uintptr_t)runtime,
                       (uintptr_t)event, (uintptr_t)network_published) < 0) {
        fail_control_queue(runtime, "Wi-Fi");
    }
}

static bool config_is_valid(const mybot_config_t *cfg) {
    return cfg && memchr(cfg->server_base, '\0', sizeof(cfg->server_base)) &&
           memchr(cfg->device_id, '\0', sizeof(cfg->device_id)) &&
           memchr(cfg->firmware_ver, '\0', sizeof(cfg->firmware_ver)) &&
           memchr(cfg->hw_model, '\0', sizeof(cfg->hw_model)) && cfg->server_base[0] &&
           cfg->device_id[0];
}

static bool platform_requirements_are_met(const mybot_config_t *cfg) {
    if (!mybot_platform_registry_is_registered()) {
        AOSL_LOG_ERR("platform descriptor is not registered");
        return false;
    }
#if MYBOT_WAKE_WORDS
    if (!mybot_platform_registry_get()->wake_words) {
        AOSL_LOG_ERR("wake-word platform operations are required but unavailable");
        return false;
    }
#endif
    if (strncmp(cfg->server_base, "https://", 8) == 0 && !mybot_platform_registry_get()->https) {
        AOSL_LOG_ERR("HTTPS platform operations are required but unavailable");
        return false;
    }
    return true;
}

static bool server_scheme_is_supported(const char *server_base) {
    if (strncmp(server_base, "https://", 8) == 0) {
#if MYBOT_ENABLE_HTTPS
        return true;
#else
        return false;
#endif
    }
    if (strncmp(server_base, "http://", 7) == 0) {
#if MYBOT_ALLOW_INSECURE_HTTP
        return true;
#else
        return false;
#endif
    }
    return false;
}

static void control_stop_runtime(mybot_runtime_t *runtime) {
    if (runtime_get_state(runtime) == MYBOT_STATE_STOPPED) {
        return;
    }

    mybot_state_t previous = runtime_get_state(runtime);
    AOSL_LOG_NTC("application control stopping");
    mybot_state_model_begin_stop(&runtime->state_model);
    runtime_publish_exit(runtime);
    if (previous != MYBOT_STATE_FAILED) {
        mybot_presenter_show_screen(&runtime->presenter, MYBOT_LCD_SCREEN_STOPPING);
    }

    cleanup_services(runtime);
    mybot_wifi_deinit(&runtime->wifi);

    mybot_presenter_show_screen(&runtime->presenter, previous == MYBOT_STATE_FAILED
                                                         ? MYBOT_LCD_SCREEN_FAILED
                                                         : MYBOT_LCD_SCREEN_STOPPING);
    mybot_presenter_deinit(&runtime->presenter);

    mybot_agora_rtc_fini();
    mybot_media_pipeline_destroy(&runtime->media);

    mybot_state_model_reset(&runtime->state_model);
    AOSL_LOG_NTC("application control stopped");
}

static void handle_control_start(const aosl_ts_t *queued_ts, aosl_refobj_t robj, uintptr_t argc,
                                 uintptr_t argv[]) {
    (void)queued_ts;
    (void)robj;
    if (argc != 1) {
        return;
    }

    mybot_runtime_t *runtime = (mybot_runtime_t *)argv[0];
    runtime->control_start_result = -1;
    if (aosl_atomic_read(&s_exit_generation) != aosl_atomic_read(&s_run_exit_generation)) {
        return;
    }
    mybot_state_model_reset(&runtime->state_model);
    if (!mybot_state_model_begin_start(&runtime->state_model)) {
        AOSL_LOG_ERR("failed to enter application start state");
        return;
    }
    aosl_atomic_set(&runtime->running, true);

    if (mybot_presenter_init(&runtime->presenter) < 0) {
        AOSL_LOG_ERR("failed to initialize application presenter");
        goto fail;
    }
    mybot_presenter_show_screen(&runtime->presenter, MYBOT_LCD_SCREEN_STARTING);
    mybot_presenter_show_screen(&runtime->presenter, MYBOT_LCD_SCREEN_WIFI_PROVISIONING);
    if (mybot_wifi_init(&runtime->wifi, runtime->config.device_id, on_wifi_event, runtime) < 0) {
        AOSL_LOG_ERR("failed to initialize Wi-Fi platform");
        goto fail;
    }
    if (aosl_atomic_read(&s_exit_generation) != aosl_atomic_read(&s_run_exit_generation)) {
        runtime_publish_exit(runtime);
        return;
    }

    runtime->control_start_result = 0;
    AOSL_LOG_NTC("application control started");
    return;

fail:
    (void)mybot_state_model_fail(&runtime->state_model);
    mybot_presenter_show_screen(&runtime->presenter, MYBOT_LCD_SCREEN_FAILED);
    runtime_publish_exit(runtime);
}

static void handle_control_stop(const aosl_ts_t *queued_ts, aosl_refobj_t robj, uintptr_t argc,
                                uintptr_t argv[]) {
    (void)queued_ts;
    (void)robj;
    if (argc == 1) {
        control_stop_runtime((mybot_runtime_t *)argv[0]);
    }
}

static void control_worker_fini(void *arg) {
    control_stop_runtime(arg);
}

static bool destroy_control_queue(mybot_runtime_t *runtime) {
    if (aosl_mpq_invalid(runtime->control_mpq)) {
        return true;
    }
    if (aosl_mpq_destroy_wait(runtime->control_mpq) < 0) {
        AOSL_LOG_ERR("failed to destroy application control queue");
        return false;
    }
    runtime->control_mpq = AOSL_MPQ_INVALID;
    return true;
}

int mybot_start(const mybot_config_t *cfg) {
    mybot_runtime_t *runtime = &s_default_runtime;

    lifecycle_lock();

    if (aosl_atomic_read(&runtime->aosl_ref_held)) {
        AOSL_LOG_ERR("application start rejected: runtime is already active");
        lifecycle_unlock();
        return -1;
    }
    if (!config_is_valid(cfg)) {
        AOSL_LOG_ERR("application start rejected: invalid configuration");
        lifecycle_unlock();
        return -1;
    }
    if (!server_scheme_is_supported(cfg->server_base)) {
        AOSL_LOG_ERR("application start rejected: unsupported server URL scheme");
        lifecycle_unlock();
        return -1;
    }

    if (!platform_requirements_are_met(cfg)) {
        lifecycle_unlock();
        return -1;
    }
    aosl_atomic_set(&s_run_exit_generation, aosl_atomic_read(&s_exit_generation));
    aosl_atomic_set(&runtime->running, false);
    memcpy(&runtime->config, cfg, sizeof(runtime->config));
    runtime->control_mpq = AOSL_MPQ_INVALID;
    runtime->control_timer = AOSL_MPQ_TIMER_INVALID;
    runtime->control_start_result = -1;

    aosl_ctor();
    aosl_atomic_set(&runtime->aosl_ref_held, true);

    runtime->control_mpq = aosl_mpq_create(AOSL_THRD_PRI_NORMAL, CONTROL_MPQ_STACK_SIZE, 1000,
                                           "control_mpq", NULL, control_worker_fini, runtime);
    if (aosl_mpq_invalid(runtime->control_mpq)) {
        AOSL_LOG_ERR("failed to create application control queue");
        goto fail;
    }
    if (aosl_mpq_call(runtime->control_mpq, AOSL_REF_INVALID, "handle_control_start",
                      handle_control_start, 1, (uintptr_t)runtime) < 0) {
        AOSL_LOG_ERR("failed to run application control startup");
        goto fail;
    }
    if (runtime->control_start_result < 0) {
        goto fail;
    }
    lifecycle_unlock();
    return 0;

fail:
    runtime_publish_exit(runtime);
    if (!aosl_mpq_invalid(runtime->control_mpq)) {
        if (aosl_mpq_call(runtime->control_mpq, AOSL_REF_INVALID, "handle_control_stop",
                          handle_control_stop, 1, (uintptr_t)runtime) < 0) {
            AOSL_LOG_ERR("failed to run application control cleanup");
        }
    }
    if (destroy_control_queue(runtime)) {
        aosl_dtor();
        aosl_atomic_set(&runtime->aosl_ref_held, false);
    }
    lifecycle_unlock();
    return -1;
}

bool mybot_is_running(void) {
    return runtime_is_running(&s_default_runtime);
}

mybot_state_t mybot_get_state(void) {
    return runtime_get_state(&s_default_runtime);
}

void mybot_stop(void) {
    mybot_runtime_t *runtime = &s_default_runtime;
    lifecycle_lock();
    if (!aosl_atomic_read(&runtime->aosl_ref_held)) {
        lifecycle_unlock();
        return;
    }

    runtime_publish_exit(runtime);
    if (!aosl_mpq_invalid(runtime->control_mpq)) {
        if (aosl_mpq_call(runtime->control_mpq, AOSL_REF_INVALID, "handle_control_stop",
                          handle_control_stop, 1, (uintptr_t)runtime) < 0) {
            AOSL_LOG_ERR("failed to run application control shutdown");
        }
    }
    if (destroy_control_queue(runtime)) {
        aosl_dtor();
        aosl_atomic_set(&runtime->aosl_ref_held, false);
    }
    lifecycle_unlock();
}
