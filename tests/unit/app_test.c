/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/mybot.h>
#include <mybot/mybot_build_config.h>
#include <mybot/platform/mybot_audio.h>
#include <mybot/platform/mybot_key.h>
#include <mybot/platform/mybot_lcd.h>
#include <mybot/platform/mybot_platform.h>
#include <mybot/platform/mybot_video.h>
#include <mybot/platform/mybot_wake_words.h>
#include <mybot/platform/mybot_wifi.h>

#include "mybot_announce_internal.h"
#include "mybot_audio_internal.h"
#include "mybot_device_lifecycle.h"
#include "mybot_key_internal.h"
#include "mybot_kv_store_internal.h"
#include "mybot_json.h"
#include "mybot_lcd_internal.h"
#include "mybot_platform_registry.h"
#include "mybot_agora_rtc.h"
#include "mybot_wake_words_internal.h"
#include "mybot_wifi_internal.h"

#include <api/aosl.h>
#include <api/aosl_log.h>
#include <api/aosl_mpq.h>
#include <hal/aosl_hal_memory.h>
#include <hal/aosl_hal_time.h>

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TEST_SAMPLE_RATE 16000
#define TEST_CHANNELS 1
#define TEST_BITS_PER_SAMPLE 16
#define TEST_FRAME_SAMPLES (TEST_SAMPLE_RATE * MYBOT_AUDIO_PTIME_MS / 1000)
#define TEST_MONO_FRAME_BYTES (TEST_FRAME_SAMPLES * sizeof(int16_t))
#if MYBOT_CLOUD_AEC
#define TEST_SEND_STREAMS 2
#else
#define TEST_SEND_STREAMS 1
#endif
#define TEST_SEND_FRAME_BYTES (TEST_MONO_FRAME_BYTES * TEST_SEND_STREAMS)
#define TEST_MAX_SENT_FRAMES 64
#define TEST_MIC_BASE 1000
#define TEST_REF_SAMPLE (-2345)

#if MYBOT_ENABLE_HTTPS
#define TEST_SERVER_BASE "https://server"
#else
#define TEST_SERVER_BASE "http://server"
#endif

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_io_cond = PTHREAD_COND_INITIALIZER;

static mybot_wifi_event_handler_t s_wifi_handler;
static void *s_wifi_user_data;
static mybot_key_event_handler_t s_key_handler;
static void *s_key_user_data;
static mybot_wake_words_handler_t s_wake_word_handler;
static void *s_wake_word_user_data;
static mybot_device_lifecycle_t *s_device_lifecycle;

static mybot_device_lifecycle_callbacks_t s_device_callbacks;
static mybot_device_state_t s_device_state = MYBOT_DEVICE_STATE_RUNTIME;
static bool s_device_start_requested;
static bool s_device_stop_requested;
static bool s_device_pair_requested;
static bool s_device_token_renewal_requested;
static bool s_device_network_available = true;
static bool s_service_poll_requested;
static bool s_block_service_poll;
static bool s_service_poll_entered;
static bool s_block_service_stop;
static bool s_service_stop_entered;
static int s_service_poll_calls;
static int s_service_stop_calls;
static int s_service_stop_result;
static int s_service_result_calls;
static bool s_track_service_thread;
static bool s_service_thread_valid;
static pthread_t s_service_thread;
static aosl_mpq_t s_test_service_mpq = AOSL_MPQ_INVALID;
static bool s_block_device_tick;
static bool s_device_tick_entered;
static bool s_rtc_flood_returned;
static aosl_mpq_t s_test_rtc_mpq = AOSL_MPQ_INVALID;
static aosl_mpq_t s_test_control_mpq = AOSL_MPQ_INVALID;
static bool s_announce_active;
static bool s_wifi_init_fails;
static bool s_wifi_emit_connected_on_init;
static bool s_block_wifi_init;
static bool s_wifi_init_entered;
static bool s_start_thread_returned;
static bool s_stop_thread_entered;
static bool s_stop_thread_returned;
static int s_start_thread_result;
static bool s_kv_init_fails;
static int s_rtc_init_result;
static int s_rtc_join_result;
static bool s_lcd_init_fails;
enum {
    SERVICE_FAIL_NONE,
    SERVICE_FAIL_KEY_INIT,
    SERVICE_FAIL_CAPTURE_INIT,
    SERVICE_FAIL_CAPTURE_START,
    SERVICE_FAIL_PLAYBACK_INIT,
    SERVICE_FAIL_PLAYBACK_START,
    SERVICE_FAIL_DEVICE_INIT,
};
static int s_service_failure;
#if MYBOT_TEST_WRAP_APP_MPQ
static bool s_control_create_fails;
static bool s_control_call_fails;
static bool s_control_destroy_fails;
static bool s_service_create_fails;
static bool s_service_destroy_fails;
static const char *s_media_create_failure;
static const char *s_media_destroy_failure;
static aosl_mpq_t s_media_unjoinable = AOSL_MPQ_INVALID;
static aosl_mpq_t s_wrapped_control_mpq = AOSL_MPQ_INVALID;
static aosl_mpq_t s_wrapped_service_mpq = AOSL_MPQ_INVALID;
static int s_control_destroy_failures;
static int s_service_destroy_failures;
static int s_service_destroy_calls;
#endif

static mybot_agora_rtc_callbacks_t s_rtc_callbacks;
static bool s_rtc_initialized;
static bool s_rtc_joined;
static bool s_platform_registered;
static bool s_https_registered;
static bool s_wake_words_registered;

static const mybot_https_ops_t s_registered_https_ops;
static const mybot_wake_words_ops_t s_registered_wake_words_ops;
static void mock_lock(void);
static void mock_unlock(void);
static void observe_control_thread(unsigned int observation);
#if MYBOT_ENABLE_VIDEO
static int s_video_context;
static pthread_t s_video_owner;
static bool s_block_video_start;
static bool s_video_start_entered;
static bool s_video_start_in_progress;
static aosl_atomic_t s_video_init_fails;
static aosl_atomic_t s_video_stop_fails;
static aosl_atomic_t s_video_init_calls;
static aosl_atomic_t s_video_start_calls;
static aosl_atomic_t s_video_stop_calls;
static aosl_atomic_t s_video_destroy_calls;
static aosl_atomic_t s_video_key_frame_calls;
static aosl_atomic_t s_video_bitrate_calls;

static int video_init(void **ctx, mybot_video_frame_handler_t handler, void *user_data) {
    assert(handler != NULL && user_data != NULL);
    observe_control_thread(0);
    s_video_owner = pthread_self();
    aosl_atomic_inc(&s_video_init_calls);
    if (aosl_atomic_read(&s_video_init_fails)) {
        return -1;
    }
    *ctx = &s_video_context;
    return 0;
}

static int video_start(void *ctx) {
    assert(ctx == &s_video_context);
    assert(pthread_equal(s_video_owner, pthread_self()));
    observe_control_thread(0);
    mock_lock();
    s_video_start_in_progress = true;
    s_video_start_entered = true;
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 3;
    while (s_block_video_start) {
        assert(pthread_cond_timedwait(&s_io_cond, &s_lock, &deadline) == 0);
    }
    s_video_start_in_progress = false;
    mock_unlock();
    aosl_atomic_inc(&s_video_start_calls);
    return 0;
}

static int video_stop(void *ctx) {
    assert(ctx == &s_video_context);
    assert(pthread_equal(s_video_owner, pthread_self()));
    observe_control_thread(0);
    mock_lock();
    assert(!s_video_start_in_progress);
    mock_unlock();
    aosl_atomic_inc(&s_video_stop_calls);
    return aosl_atomic_read(&s_video_stop_fails) ? -1 : 0;
}

static void video_target_bitrate(void *ctx, uint32_t target_bps) {
    assert(ctx == &s_video_context);
    assert(pthread_equal(s_video_owner, pthread_self()));
    observe_control_thread(0);
    assert(target_bps == 96000U);
    aosl_atomic_inc(&s_video_bitrate_calls);
}

static void video_key_frame(void *ctx) {
    assert(ctx == &s_video_context);
    assert(pthread_equal(s_video_owner, pthread_self()));
    observe_control_thread(0);
    aosl_atomic_inc(&s_video_key_frame_calls);
}

static void video_destroy(void *ctx) {
    assert(ctx == &s_video_context);
    assert(pthread_equal(s_video_owner, pthread_self()));
    observe_control_thread(0);
    aosl_atomic_inc(&s_video_destroy_calls);
}

static const mybot_video_ops_t s_registered_video_ops = {
    .min_bps = 32000,
    .max_bps = 256000,
    .init = video_init,
    .start = video_start,
    .stop = video_stop,
    .on_target_bitrate_changed = video_target_bitrate,
    .on_key_frame_request = video_key_frame,
    .destroy = video_destroy,
};
#endif
static mybot_platform_descriptor_t s_registry_view;

static int s_wifi_init_calls;
static int s_wifi_deinit_calls;
static int s_kv_init_calls;
static int s_kv_deinit_calls;
static int s_key_init_calls;
static int s_key_deinit_calls;
static int s_capture_init_calls;
static int s_capture_start_calls;
static int s_capture_stop_calls;
static int s_capture_destroy_calls;
static int s_playback_init_calls;
static int s_playback_start_calls;
static int s_playback_stop_calls;
static int s_playback_destroy_calls;
static int s_playback_write_calls;
static int s_last_playback_sample;
static int s_announce_active_checks;
static int s_device_init_calls;
static int s_device_shutdown_calls;
static int s_network_down_calls;
static int s_network_up_calls;
static int s_network_set_calls;
static int s_rtc_init_calls;
static int s_rtc_join_calls;
static int s_rtc_leave_calls;
static int s_rtc_fini_calls;
static bool s_rtc_fini_retry;
static int s_rtc_send_calls;
static int s_rtc_renew_calls;
static int s_token_renewal_requests;
static int s_pair_requests;
static int s_pair_callback_calls;
static int s_stop_requests;
static int s_conversation_ended_notifications;
static int s_media_volume_set_calls;
static int s_last_media_volume;
static char s_renewed_token[512];

static size_t s_sent_lengths[TEST_MAX_SENT_FRAMES];
static int16_t s_sent_frames[TEST_MAX_SENT_FRAMES][TEST_SEND_FRAME_BYTES / sizeof(int16_t)];
static char s_join_channel[128];
static char s_join_user[64];
static mybot_lcd_content_t s_last_lcd_content;

enum {
    CONTROL_OBS_KV_INIT = 1u << 0,
    CONTROL_OBS_KEY_INIT = 1u << 1,
    CONTROL_OBS_CAPTURE_INIT = 1u << 2,
    CONTROL_OBS_PLAYBACK_INIT = 1u << 3,
    CONTROL_OBS_DEVICE_INIT = 1u << 4,
    CONTROL_OBS_DEVICE_TICK = 1u << 5,
    CONTROL_OBS_RTC_INIT = 1u << 6,
    CONTROL_OBS_RTC_JOIN = 1u << 7,
    CONTROL_OBS_RTC_LEAVE = 1u << 8,
    CONTROL_OBS_RTC_RENEW = 1u << 9,
    CONTROL_OBS_VOLUME = 1u << 10,
};

#define CONTROL_OBS_REQUIRED                                                                       \
    (CONTROL_OBS_KV_INIT | CONTROL_OBS_KEY_INIT | CONTROL_OBS_CAPTURE_INIT |                       \
     CONTROL_OBS_PLAYBACK_INIT | CONTROL_OBS_DEVICE_INIT | CONTROL_OBS_DEVICE_TICK |               \
     CONTROL_OBS_RTC_INIT | CONTROL_OBS_RTC_JOIN | CONTROL_OBS_RTC_LEAVE | CONTROL_OBS_RTC_RENEW | \
     CONTROL_OBS_VOLUME)

static bool s_track_control_thread;
static bool s_control_thread_valid;
static bool s_control_thread_mismatch;
static pthread_t s_control_thread;
static unsigned int s_control_observations;

static int s_capture_context;
static int s_playback_context;
static bool s_capture_started;
static bool s_playback_started;
static int s_playback_write_limit = -1;
static bool s_block_capture_read;
static bool s_capture_read_blocked;
static bool s_capture_read_timed_out;
static bool s_block_playback_write;
static bool s_playback_write_blocked;
static bool s_playback_write_timed_out;

static void mock_lock(void) {
    assert(pthread_mutex_lock(&s_lock) == 0);
}

static void mock_unlock(void) {
    assert(pthread_mutex_unlock(&s_lock) == 0);
}

#if MYBOT_TEST_WRAP_APP_MPQ
aosl_mpq_t __real_aosl_mpq_create_flags(int flags, int pri, int stack_size, int max,
                                        const char *name, aosl_mpq_init_t init,
                                        aosl_mpq_fini_t fini, void *arg);
int __real_aosl_mpq_destroy_wait(aosl_mpq_t mpq);

aosl_mpq_t __wrap_aosl_mpq_create_flags(int flags, int pri, int stack_size, int max,
                                        const char *name, aosl_mpq_init_t init,
                                        aosl_mpq_fini_t fini, void *arg) {
    bool control = name && strcmp(name, "control_mpq") == 0;
    bool service = name && strcmp(name, "service_mpq") == 0;
    if (control && s_control_create_fails) {
        return AOSL_MPQ_INVALID;
    }
    if (service && s_service_create_fails) {
        return AOSL_MPQ_INVALID;
    }
    if (s_media_create_failure && name && strcmp(name, s_media_create_failure) == 0) {
        return AOSL_MPQ_INVALID;
    }
    aosl_mpq_t mpq =
        __real_aosl_mpq_create_flags(flags, pri, stack_size, max, name, init, fini, arg);
    if (control) {
        s_wrapped_control_mpq = mpq;
    }
    if (service) {
        s_wrapped_service_mpq = mpq;
    }
    if (s_media_destroy_failure && name && strcmp(name, s_media_destroy_failure) == 0) {
        s_media_unjoinable = mpq;
    }
    return mpq;
}

int __wrap_aosl_mpq_call(aosl_mpq_t mpq, aosl_ref_t ref, const char *name,
                         aosl_mpq_func_argv_t func, uintptr_t argc, ...) {
    if (name && strcmp(name, "handle_control_start") == 0 && s_control_call_fails) {
        return -1;
    }
    va_list args;
    va_start(args, argc);
    int result = aosl_mpq_call_args(mpq, ref, name, func, argc, args);
    va_end(args);
    return result;
}

int __wrap_aosl_mpq_destroy_wait(aosl_mpq_t mpq) {
    if (mpq == s_media_unjoinable) {
        return -1;
    }
    if (mpq == s_wrapped_control_mpq && s_control_destroy_fails) {
        s_control_destroy_failures++;
        return -1;
    }
    if (mpq == s_wrapped_service_mpq) {
        mock_lock();
        s_service_destroy_calls++;
        assert(pthread_cond_broadcast(&s_io_cond) == 0);
        bool fails = s_service_destroy_fails;
        if (fails) {
            s_service_destroy_failures++;
        }
        mock_unlock();
        if (fails) {
            return -1;
        }
    }
    return __real_aosl_mpq_destroy_wait(mpq);
}
#endif

static void observe_control_thread(unsigned int observation) {
    pthread_t current = pthread_self();

    mock_lock();
    if (s_track_control_thread) {
        if (!s_control_thread_valid) {
            s_control_thread = current;
            s_control_thread_valid = true;
        } else if (!pthread_equal(s_control_thread, current)) {
            s_control_thread_mismatch = true;
        }
        s_control_observations |= observation;
    }
    mock_unlock();
}

static void begin_control_thread_tracking(void) {
    mock_lock();
    s_track_control_thread = true;
    s_control_thread_valid = false;
    s_control_thread_mismatch = false;
    s_control_observations = 0;
    mock_unlock();
}

static void end_control_thread_tracking(void) {
    mock_lock();
    assert(s_control_thread_valid);
    assert(!s_control_thread_mismatch);
    assert((s_control_observations & CONTROL_OBS_REQUIRED) == CONTROL_OBS_REQUIRED);
    s_track_control_thread = false;
    mock_unlock();
}

static int read_counter(const int *counter) {
    mock_lock();
    int value = *counter;
    mock_unlock();
    return value;
}

static bool read_bool(const bool *flag) {
    mock_lock();
    bool value = *flag;
    mock_unlock();
    return value;
}

static bool wait_for_flag(const bool *flag, int timeout_ms) {
    for (int elapsed = 0; elapsed < timeout_ms; elapsed++) {
        mock_lock();
        bool value = *flag;
        mock_unlock();
        if (value) {
            return true;
        }
        aosl_hal_msleep(1);
    }
    return false;
}

static void wait_for_service_flag(const bool *flag) {
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 3;
    mock_lock();
    while (!*flag) {
        assert(pthread_cond_timedwait(&s_io_cond, &s_lock, &deadline) == 0);
    }
    mock_unlock();
}

static void service_request_entered(bool *entered, const bool *blocked) {
    assert(!aosl_mpq_invalid(aosl_mpq_this()));
    mock_lock();
    s_test_service_mpq = aosl_mpq_this();
    if (s_track_service_thread) {
        assert(s_control_thread_valid);
        assert(!pthread_equal(s_control_thread, pthread_self()));
        if (s_service_thread_valid) {
            assert(pthread_equal(s_service_thread, pthread_self()));
        } else {
            s_service_thread = pthread_self();
            s_service_thread_valid = true;
        }
    }
    *entered = true;
    assert(pthread_cond_broadcast(&s_io_cond) == 0);
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 5;
    while (*blocked) {
        assert(pthread_cond_timedwait(&s_io_cond, &s_lock, &deadline) == 0);
    }
    mock_unlock();
}

int mybot_device_client_get_binding_status(const char *base_url, const char *device_id,
                                           const char *auth_header, mybot_device_binding_t *resp) {
    assert(base_url != NULL);
    assert(device_id != NULL);
    assert(auth_header != NULL);
    assert(resp != NULL);
    assert(strcmp(base_url, TEST_SERVER_BASE) == 0);
    assert(strcmp(device_id, "device-1") == 0);
    assert(strcmp(auth_header, "Device device-service-token") == 0);
    service_request_entered(&s_service_poll_entered, &s_block_service_poll);
    memset(resp, 0, sizeof(*resp));
    snprintf(resp->status, sizeof(resp->status), "%s", "bound");
    mock_lock();
    s_service_poll_calls++;
    assert(pthread_cond_broadcast(&s_io_cond) == 0);
    mock_unlock();
    return 0;
}

int mybot_device_client_stop_conversation(const char *base_url, const char *device_id,
                                          const char *device_token, const char *conversation_id,
                                          const char *reason) {
    assert(base_url != NULL);
    assert(device_id != NULL);
    assert(device_token != NULL);
    assert(conversation_id != NULL);
    assert(reason != NULL);
    assert(strcmp(base_url, TEST_SERVER_BASE) == 0);
    assert(strcmp(device_id, "device-1") == 0);
    assert(strcmp(device_token, "device-service-token") == 0);
    assert(strcmp(conversation_id, "conversation-1") == 0);
    assert(strcmp(reason, MYBOT_CONVERSATION_STOP_REASON_USER_REQUESTED) == 0 ||
           strcmp(reason, MYBOT_CONVERSATION_STOP_REASON_DEVICE_HANGUP) == 0);
    service_request_entered(&s_service_stop_entered, &s_block_service_stop);
    mock_lock();
    s_service_stop_calls++;
    int result = s_service_stop_result;
    assert(pthread_cond_broadcast(&s_io_cond) == 0);
    mock_unlock();
    return result;
}

static void wait_for_io_stop(bool *block_requested, bool *blocked, bool *started, bool *timed_out) {
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 5;
    *blocked = true;
    assert(pthread_cond_broadcast(&s_io_cond) == 0);
    while (*block_requested && *started) {
        int ret = pthread_cond_timedwait(&s_io_cond, &s_lock, &deadline);
        if (ret == ETIMEDOUT) {
            *timed_out = true;
            *block_requested = false;
            break;
        }
        assert(ret == 0);
    }
    *blocked = false;
}

static bool wait_for_app_state(mybot_state_t expected, int timeout_ms) {
    for (int elapsed = 0; elapsed < timeout_ms; elapsed++) {
        if (mybot_get_state() == expected) {
            return true;
        }
        aosl_hal_msleep(1);
    }
    return false;
}

static bool wait_for_runtime_exit(int timeout_ms) {
    for (int elapsed = 0; elapsed < timeout_ms; ++elapsed) {
        if (!mybot_is_running()) {
            return true;
        }
        aosl_hal_msleep(1);
    }
    return false;
}

static bool wait_for_counter(const int *counter, int minimum, int timeout_ms) {
    for (int elapsed = 0; elapsed < timeout_ms; elapsed++) {
        if (read_counter(counter) >= minimum) {
            return true;
        }
        aosl_hal_msleep(1);
    }
    return false;
}

#if MYBOT_ENABLE_VIDEO
static bool wait_for_video_counter(aosl_atomic_t *counter, intptr_t minimum) {
    for (int elapsed = 0; elapsed < 1000; elapsed++) {
        if (aosl_atomic_read(counter) >= minimum) {
            return true;
        }
        aosl_hal_msleep(1);
    }
    return false;
}

static void *rtc_connected_thread(void *arg) {
    (void)arg;
    mock_lock();
    mybot_agora_rtc_callbacks_t callbacks = s_rtc_callbacks;
    mock_unlock();
    assert(callbacks.on_state_changed != NULL);
    callbacks.on_state_changed(MYBOT_RTC_STATE_CONNECTED, callbacks.user_data);
    return NULL;
}

static void emit_rtc_connected(void) {
    pthread_t thread;
    assert(pthread_create(&thread, NULL, rtc_connected_thread, NULL) == 0);
    assert(pthread_join(thread, NULL) == 0);
}

#endif

static bool wait_for_flag_value(const bool *flag, bool expected, int timeout_ms) {
    for (int elapsed = 0; elapsed < timeout_ms; elapsed++) {
        mock_lock();
        bool value = *flag;
        mock_unlock();
        if (value == expected) {
            return true;
        }
        aosl_hal_msleep(1);
    }
    return false;
}

static bool wait_for_lcd_indicator(uint32_t indicator, int timeout_ms) {
    for (int elapsed = 0; elapsed < timeout_ms; elapsed++) {
        mock_lock();
        bool matched = (s_last_lcd_content.indicators & indicator) != 0;
        mock_unlock();
        if (matched) {
            return true;
        }
        aosl_hal_msleep(1);
    }
    return false;
}

static bool wait_for_lcd_indicator_state(uint32_t indicator, bool expected, int timeout_ms) {
    for (int elapsed = 0; elapsed < timeout_ms; elapsed++) {
        mock_lock();
        bool matched = ((s_last_lcd_content.indicators & indicator) != 0) == expected;
        mock_unlock();
        if (matched) {
            return true;
        }
        aosl_hal_msleep(1);
    }
    return false;
}

static int16_t expected_mic_sample(int index) {
    return (int16_t)(TEST_MIC_BASE + index % 200);
}

static bool sent_frame_matches(int index, int16_t reference_sample) {
    bool matches = true;
    mock_lock();
    if (index < 0 || index >= s_rtc_send_calls || index >= TEST_MAX_SENT_FRAMES ||
        s_sent_lengths[index] != TEST_SEND_FRAME_BYTES) {
        matches = false;
    } else {
        const int16_t *pcm = (const int16_t *)s_sent_frames[index];
        for (int i = 0; i < TEST_FRAME_SAMPLES; i++) {
#if MYBOT_CLOUD_AEC
            if (pcm[i * 2] != expected_mic_sample(i) || pcm[i * 2 + 1] != reference_sample) {
#else
            (void)reference_sample;
            if (pcm[i] != expected_mic_sample(i)) {
#endif
                matches = false;
                break;
            }
        }
    }
    mock_unlock();
    return matches;
}

static bool wait_for_audio_frame_from(int16_t reference_sample, int first_frame, int timeout_ms) {
    for (int elapsed = 0; elapsed < timeout_ms; elapsed++) {
        int send_calls = read_counter(&s_rtc_send_calls);
        int limit = send_calls < TEST_MAX_SENT_FRAMES ? send_calls : TEST_MAX_SENT_FRAMES;
        for (int i = first_frame; i < limit; i++) {
            if (sent_frame_matches(i, reference_sample)) {
                return true;
            }
        }
        aosl_hal_msleep(1);
    }
    return false;
}

static bool wait_for_audio_frame(int16_t reference_sample, int timeout_ms) {
    return wait_for_audio_frame_from(reference_sample, 0, timeout_ms);
}

static void emit_key_event(mybot_key_event_t event) {
    mock_lock();
    mybot_key_event_handler_t handler = s_key_handler;
    void *user_data = s_key_user_data;
    mock_unlock();
    assert(handler != NULL);
    handler(event, user_data);
}

static void wait_for_control_events(void) {
    /* The following volume event runs after already queued RTM notifications. */
    int volume_calls = read_counter(&s_media_volume_set_calls);
    emit_key_event(MYBOT_KEY_EVENT_VOLUME_UP);
    assert(wait_for_counter(&s_media_volume_set_calls, volume_calls + 1, 1000));
}

static void emit_wifi_event(mybot_wifi_event_t event) {
    mock_lock();
    mybot_wifi_event_handler_t handler = s_wifi_handler;
    void *user_data = s_wifi_user_data;
    mock_unlock();
    assert(handler != NULL);
    handler(event, user_data);
}

#if MYBOT_WAKE_WORDS
static void emit_wake_word(void) {
    mock_lock();
    mybot_wake_words_handler_t handler = s_wake_word_handler;
    void *user_data = s_wake_word_user_data;
    mock_unlock();
    assert(handler != NULL);
    handler("hello", user_data);
}
#endif

static void emit_remote_audio(const int16_t *pcm, size_t len) {
    mock_lock();
    void (*callback)(uint32_t, const void *, size_t, void *) = s_rtc_callbacks.on_remote_audio;
    void *user_data = s_rtc_callbacks.user_data;
    mock_unlock();
    assert(callback != NULL);
    callback(7, pcm, len, user_data);
}

static void emit_rtm_data(const char *rtm_uid, const void *data, size_t len) {
    mock_lock();
    void (*callback)(const char *, const void *, size_t, const char *, void *) =
        s_rtc_callbacks.on_rtm_data;
    void *user_data = s_rtc_callbacks.user_data;
    mock_unlock();
    assert(callback != NULL);
    callback(rtm_uid, data, len, "json", user_data);
}

static void emit_rtm_subscribe_data(const char *channel, const char *rtm_uid, const void *data,
                                    size_t len) {
    mock_lock();
    void (*callback)(const char *, const char *, const void *, size_t, const char *, void *) =
        s_rtc_callbacks.on_rtm_subscribe_data;
    void *user_data = s_rtc_callbacks.user_data;
    mock_unlock();
    assert(callback != NULL);
    callback(channel, rtm_uid, data, len, "json", user_data);
}

static void control_barrier(const aosl_ts_t *ts, aosl_refobj_t ref, uintptr_t argc,
                            uintptr_t argv[]) {
    (void)ts;
    (void)ref;
    (void)argv;
    assert(argc == 0);
    observe_control_thread(0);
}

static void drain_device_commands(void) {
    mock_lock();
    aosl_mpq_t control = s_test_control_mpq;
    mock_unlock();
    assert(!aosl_mpq_invalid(control));
    assert(aosl_mpq_call(control, AOSL_REF_INVALID, "device_command_test_barrier", control_barrier,
                         0) == 0);
}

static size_t make_cloud_message(char *message, size_t capacity, const char *fields,
                                 const char *content_key, const char *content);

static size_t make_device_command(char *message, size_t capacity, const char *id,
                                  const char *conversation, const char *command,
                                  const char *metadata) {
    int length = snprintf(message, capacity,
                          "{\"type\":\"device_command\",\"schema\":\"botstation.device-command\","
                          "\"version\":1,\"message_id\":\"msg-%s\",\"command_id\":\"%s\","
                          "\"conversation_id\":\"%s\",\"command\":%s%s}",
                          id, id, conversation, command, metadata ? metadata : "");
    assert(length > 0 && (size_t)length < capacity);
    return (size_t)length;
}

static void emit_bare_device_command(const char *channel, const char *sender, const char *id,
                                     const char *conversation, const char *command) {
    char message[768];
    size_t length = make_device_command(message, sizeof(message), id, conversation, command, NULL);
    if (channel) {
        emit_rtm_subscribe_data(channel, sender, message, length);
    } else {
        emit_rtm_data(sender, message, length);
    }
}

static void emit_device_command(const char *channel, const char *sender, const char *id,
                                const char *conversation, const char *command,
                                const char *metadata) {
    char content[768];
    make_device_command(content, sizeof(content), id, conversation, command, metadata);
    char message[1024];
    size_t length = make_cloud_message(message, sizeof(message),
                                       "\"object\":\"message.user\",\"data_type\":\"message\"",
                                       "content", content);
    if (channel) {
        emit_rtm_subscribe_data(channel, sender, message, (size_t)length);
    } else {
        emit_rtm_data(sender, message, (size_t)length);
    }
}

static const char s_volume_command[] =
    "{\"type\":\"action\",\"name\":\"audio.set_volume\",\"params\":{\"level\":50}}";

bool mybot_platform_registry_is_registered(void) {
    return s_platform_registered;
}

const mybot_platform_descriptor_t *mybot_platform_registry_get(void) {
    s_registry_view.https = s_https_registered ? &s_registered_https_ops : NULL;
    s_registry_view.wake_words = s_wake_words_registered ? &s_registered_wake_words_ops : NULL;
#if MYBOT_ENABLE_VIDEO
    s_registry_view.video = s_platform_registered ? &s_registered_video_ops : NULL;
#endif
    return &s_registry_view;
}

bool mybot_lcd_is_registered(void) {
    return true;
}

int mybot_lcd_init(mybot_lcd_t *lcd) {
    assert(lcd != NULL);
    return s_lcd_init_fails ? -1 : 0;
}

int mybot_lcd_show_content(mybot_lcd_t *lcd, const mybot_lcd_content_t *content) {
    assert(lcd != NULL);
    assert(content != NULL);
    mock_lock();
    s_last_lcd_content = *content;
    mock_unlock();
    return 0;
}

int mybot_lcd_show_pair_code(mybot_lcd_t *lcd, const char *pair_code) {
    assert(lcd != NULL);
    (void)pair_code;
    return 0;
}

void mybot_lcd_deinit(mybot_lcd_t *lcd) {
    assert(lcd != NULL);
}

int mybot_kv_store_init(mybot_kv_store_t *store) {
    assert(store != NULL);
    observe_control_thread(CONTROL_OBS_KV_INIT);
    mock_lock();
    s_kv_init_calls++;
    bool fails = s_kv_init_fails;
    mock_unlock();
    return fails ? -1 : 0;
}

void mybot_kv_store_deinit(mybot_kv_store_t *store) {
    assert(store != NULL);
    observe_control_thread(0);
    mock_lock();
    s_kv_deinit_calls++;
    mock_unlock();
}

int mybot_key_init(mybot_key_t *key, mybot_key_event_handler_t handler, void *user_data) {
    assert(key != NULL);
    observe_control_thread(CONTROL_OBS_KEY_INIT);
    mock_lock();
    s_key_handler = handler;
    s_key_user_data = user_data;
    s_key_init_calls++;
    bool fails = s_service_failure == SERVICE_FAIL_KEY_INIT;
    mock_unlock();
    return fails ? -1 : 0;
}

void mybot_key_deinit(mybot_key_t *key) {
    assert(key != NULL);
    observe_control_thread(0);
    mock_lock();
    s_key_handler = NULL;
    s_key_user_data = NULL;
    s_key_deinit_calls++;
    mock_unlock();
}

int mybot_announce_init(mybot_announce_t *announce) {
    assert(announce != NULL);
    return 0;
}

void mybot_announce_deinit(mybot_announce_t *announce) {
    assert(announce != NULL);
}

int mybot_announce_play_pair_code(mybot_announce_t *announce, const char *code) {
    assert(announce != NULL);
    (void)code;
    mock_lock();
    announce->generation++;
    mock_unlock();
    return 0;
}

void mybot_announce_stop(mybot_announce_t *announce) {
    assert(announce != NULL);
    mock_lock();
    announce->generation++;
    mock_unlock();
}

bool mybot_announce_is_active(mybot_announce_t *announce) {
    assert(announce != NULL);
    mock_lock();
    s_announce_active_checks++;
    bool active = s_announce_active;
    mock_unlock();
    return active;
}

uint32_t mybot_announce_get_generation(mybot_announce_t *announce) {
    assert(announce != NULL);
    mock_lock();
    uint32_t generation = announce->generation;
    mock_unlock();
    return generation;
}

int mybot_announce_read_pcm(mybot_announce_t *announce, int16_t *dst, int max_frames,
                            uint32_t *generation) {
    assert(announce != NULL);
    (void)dst;
    (void)max_frames;
    if (generation) {
        *generation = mybot_announce_get_generation(announce);
    }
    return 0;
}

static int capture_init(void **ctx, int rate, int channels, int bits) {
    assert(rate == TEST_SAMPLE_RATE);
    assert(channels == TEST_CHANNELS);
    assert(bits == TEST_BITS_PER_SAMPLE);
    observe_control_thread(CONTROL_OBS_CAPTURE_INIT);
    mock_lock();
    s_capture_init_calls++;
    bool fails = s_service_failure == SERVICE_FAIL_CAPTURE_INIT;
    mock_unlock();
    if (!fails) {
        *ctx = &s_capture_context;
    }
    return fails ? -1 : 0;
}

static int capture_start(void *ctx) {
    assert(ctx == &s_capture_context);
    mock_lock();
    bool fails = s_service_failure == SERVICE_FAIL_CAPTURE_START;
    s_capture_started = !fails;
    s_capture_start_calls++;
    mock_unlock();
    return fails ? -1 : 0;
}

static int capture_read(void *ctx, void *buf, int frames) {
    assert(ctx == &s_capture_context);
    mock_lock();
    if (s_block_capture_read) {
        wait_for_io_stop(&s_block_capture_read, &s_capture_read_blocked, &s_capture_started,
                         &s_capture_read_timed_out);
    }
    bool started = s_capture_started;
    mock_unlock();
    if (!started) {
        return 0;
    }
    int16_t *pcm = buf;
    for (int i = 0; i < frames; i++) {
        pcm[i] = expected_mic_sample(i);
    }
    return frames;
}

static int capture_stop(void *ctx) {
    assert(ctx == &s_capture_context);
    observe_control_thread(0);
    mock_lock();
    s_capture_started = false;
    s_block_capture_read = false;
    assert(pthread_cond_broadcast(&s_io_cond) == 0);
    s_capture_stop_calls++;
    mock_unlock();
    return 0;
}

static void capture_destroy(void *ctx) {
    assert(ctx == &s_capture_context);
    observe_control_thread(0);
    mock_lock();
    s_capture_destroy_calls++;
    mock_unlock();
}

static int playback_init(void **ctx, int rate, int channels, int bits) {
    assert(rate == TEST_SAMPLE_RATE);
    assert(channels == TEST_CHANNELS);
    assert(bits == TEST_BITS_PER_SAMPLE);
    observe_control_thread(CONTROL_OBS_PLAYBACK_INIT);
    mock_lock();
    s_playback_init_calls++;
    bool fails = s_service_failure == SERVICE_FAIL_PLAYBACK_INIT;
    mock_unlock();
    if (!fails) {
        *ctx = &s_playback_context;
    }
    return fails ? -1 : 0;
}

static int playback_start(void *ctx) {
    assert(ctx == &s_playback_context);
    mock_lock();
    bool fails = s_service_failure == SERVICE_FAIL_PLAYBACK_START;
    s_playback_started = !fails;
    s_playback_start_calls++;
    mock_unlock();
    return fails ? -1 : 0;
}

static int playback_write(void *ctx, const void *buf, int frames) {
    assert(ctx == &s_playback_context);
    assert(buf != NULL);
    mock_lock();
    if (s_block_playback_write) {
        wait_for_io_stop(&s_block_playback_write, &s_playback_write_blocked, &s_playback_started,
                         &s_playback_write_timed_out);
    }
    bool started = s_playback_started;
    int written = frames;
    if (s_playback_write_limit >= 0 && written > s_playback_write_limit) {
        written = s_playback_write_limit;
    }
    if (started) {
        s_playback_write_calls++;
        s_last_playback_sample = ((const int16_t *)buf)[0];
    }
    mock_unlock();
    return started ? written : 0;
}

static int playback_stop(void *ctx) {
    assert(ctx == &s_playback_context);
    observe_control_thread(0);
    mock_lock();
    s_playback_started = false;
    s_block_playback_write = false;
    assert(pthread_cond_broadcast(&s_io_cond) == 0);
    s_playback_stop_calls++;
    mock_unlock();
    return 0;
}

static void playback_destroy(void *ctx) {
    assert(ctx == &s_playback_context);
    observe_control_thread(0);
    mock_lock();
    s_playback_destroy_calls++;
    mock_unlock();
}

static const mybot_audio_capture_ops_t s_capture_ops = {
    .init = capture_init,
    .start = capture_start,
    .read = capture_read,
    .stop = capture_stop,
    .destroy = capture_destroy,
};

static const mybot_audio_playback_ops_t s_playback_ops = {
    .init = playback_init,
    .start = playback_start,
    .write = playback_write,
    .stop = playback_stop,
    .destroy = playback_destroy,
};

void mybot_audio_context_init(mybot_audio_t *audio) {
    if (!audio) {
        return;
    }
    memset(audio, 0, sizeof(*audio));
    audio->capture_ops = &s_capture_ops;
    audio->playback_ops = &s_playback_ops;
    aosl_atomic_set(&audio->media_volume, MYBOT_AUDIO_VOLUME_DEFAULT);
}

int mybot_audio_device_volume_init(mybot_audio_t *audio) {
    assert(audio != NULL);
    return -1;
}

void mybot_audio_device_volume_deinit(mybot_audio_t *audio) {
    assert(audio != NULL);
}

bool mybot_audio_device_volume_is_active(const mybot_audio_t *audio) {
    assert(audio != NULL);
    return false;
}

int mybot_audio_device_set_volume(mybot_audio_t *audio, int volume) {
    assert(audio != NULL);
    (void)volume;
    return -1;
}

int mybot_audio_device_get_volume(mybot_audio_t *audio, int *volume) {
    assert(audio != NULL);
    (void)volume;
    return -1;
}

int mybot_audio_set_media_volume(mybot_audio_t *audio, int volume) {
    assert(audio != NULL);
    if (volume < MYBOT_AUDIO_VOLUME_MIN || volume > MYBOT_AUDIO_VOLUME_MAX) {
        return -1;
    }
    observe_control_thread(CONTROL_OBS_VOLUME);
    aosl_atomic_set(&audio->media_volume, volume);
    mock_lock();
    s_media_volume_set_calls++;
    s_last_media_volume = volume;
    mock_unlock();
    return 0;
}

int mybot_audio_get_media_volume(const mybot_audio_t *audio) {
    assert(audio != NULL);
    return (int)aosl_atomic_read(&audio->media_volume);
}

void mybot_audio_apply_media_volume(const mybot_audio_t *audio, int16_t *pcm, int samples) {
    assert(audio != NULL);
    (void)pcm;
    (void)samples;
}

int mybot_wake_words_init(mybot_wake_words_t *wake_words, int sample_rate, int channels,
                          int bits_per_sample, mybot_wake_words_handler_t handler,
                          void *user_data) {
    assert(wake_words != NULL);
    assert(handler != NULL);
    (void)sample_rate;
    (void)channels;
    (void)bits_per_sample;
    mock_lock();
    s_wake_word_handler = handler;
    s_wake_word_user_data = user_data;
    mock_unlock();
    return 0;
}

int mybot_wake_words_process(mybot_wake_words_t *wake_words, const void *pcm, int frames) {
    assert(wake_words != NULL);
    (void)pcm;
    (void)frames;
    return 0;
}

void mybot_wake_words_deinit(mybot_wake_words_t *wake_words) {
    assert(wake_words != NULL);
    mock_lock();
    s_wake_word_handler = NULL;
    s_wake_word_user_data = NULL;
    mock_unlock();
}

int mybot_wifi_init(mybot_wifi_t *wifi, const char *device_id, mybot_wifi_event_handler_t handler,
                    void *user_data) {
    assert(wifi != NULL);
    observe_control_thread(0);
    if (!device_id || !handler) {
        return -1;
    }
    assert(strcmp(device_id, "device-1") == 0);
    mock_lock();
    s_wifi_handler = handler;
    s_wifi_user_data = user_data;
    s_wifi_init_calls++;
    bool fails = s_wifi_init_fails;
    bool emit_connected = s_wifi_emit_connected_on_init;
    s_wifi_init_entered = true;
    mock_unlock();
    while (true) {
        mock_lock();
        bool blocked = s_block_wifi_init;
        mock_unlock();
        if (!blocked) {
            break;
        }
        aosl_hal_msleep(1);
    }
    if (!fails && emit_connected) {
        handler(MYBOT_WIFI_EVENT_STA_CONNECTED, user_data);
    }
    return fails ? -1 : 0;
}

void mybot_wifi_deinit(mybot_wifi_t *wifi) {
    assert(wifi != NULL);
    observe_control_thread(0);
    mock_lock();
    s_wifi_handler = NULL;
    s_wifi_user_data = NULL;
    s_wifi_deinit_calls++;
    mock_unlock();
}

static void submit_mock_service_request(const mybot_device_lifecycle_callbacks_t *callbacks,
                                        mybot_device_service_request_type_t type,
                                        const char *reason) {
    observe_control_thread(0);
    assert(callbacks->submit_service_request != NULL);
    mybot_device_service_request_t *request = aosl_hal_malloc(sizeof(*request));
    assert(request != NULL);
    memset(request, 0, sizeof(*request));
    request->type = type;
    request->network_generation = 11;
    request->binding_generation = 17;
    snprintf(request->server_base, sizeof(request->server_base), "%s", TEST_SERVER_BASE);
    snprintf(request->device_id, sizeof(request->device_id), "%s", "device-1");
    snprintf(request->credential, sizeof(request->credential), "%s", "device-service-token");
    if (type == MYBOT_DEVICE_SERVICE_STOP) {
        snprintf(request->payload.stop.conversation_id,
                 sizeof(request->payload.stop.conversation_id), "%s", "conversation-1");
        snprintf(request->payload.stop.reason, sizeof(request->payload.stop.reason), "%s", reason);
    }
    assert(callbacks->submit_service_request(request, callbacks->user_data) == 0);
}

void mybot_device_lifecycle_service_result(mybot_device_lifecycle_t *lifecycle,
                                           const mybot_device_service_request_t *request) {
    assert(lifecycle == s_device_lifecycle);
    assert(request != NULL);
    assert(request->network_generation == 11);
    assert(request->binding_generation == 17);
    assert(request->type == MYBOT_DEVICE_SERVICE_STOP || request->result == 0);
    observe_control_thread(0);
    mock_lock();
    s_service_result_calls++;
    assert(pthread_cond_broadcast(&s_io_cond) == 0);
    mock_unlock();
}

int mybot_device_lifecycle_init(mybot_device_lifecycle_t *lifecycle, mybot_kv_store_t *kv_store,
                                const char *server_base, const char *device_id,
                                const char *firmware_ver, const char *hw_model,
                                const mybot_device_lifecycle_callbacks_t *callbacks) {
    if (!lifecycle || !kv_store || !server_base || !device_id || !callbacks) {
        return -1;
    }
    assert(kv_store != NULL);
    assert(strcmp(server_base, TEST_SERVER_BASE) == 0);
    assert(strcmp(device_id, "device-1") == 0);
    (void)firmware_ver;
    (void)hw_model;
    assert(callbacks != NULL);
    observe_control_thread(CONTROL_OBS_DEVICE_INIT);
    if (read_counter(&s_service_failure) == SERVICE_FAIL_DEVICE_INIT) {
        return -1;
    }
    mock_lock();
    s_device_lifecycle = lifecycle;
    lifecycle->conversation_id[0] = '\0';
    aosl_atomic_set(&lifecycle->stop_request, 0);
    s_device_callbacks = *callbacks;
    s_device_state = MYBOT_DEVICE_STATE_RUNTIME;
    s_device_start_requested = false;
    s_device_stop_requested = false;
    s_device_pair_requested = false;
    s_device_token_renewal_requested = false;
    s_device_network_available = true;
    s_service_poll_requested = false;
    s_device_init_calls++;
    mock_unlock();
    if (callbacks->on_state_changed) {
        callbacks->on_state_changed(MYBOT_DEVICE_STATE_RUNTIME, callbacks->user_data);
    }
    return 0;
}

void mybot_device_lifecycle_tick(mybot_device_lifecycle_t *lifecycle) {
    assert(lifecycle == s_device_lifecycle);
    observe_control_thread(CONTROL_OBS_DEVICE_TICK);
    mybot_device_lifecycle_callbacks_t callbacks;
    enum {
        DEVICE_TICK_IDLE = 0,
        DEVICE_TICK_PAIR,
        DEVICE_TICK_START,
        DEVICE_TICK_STOP,
        DEVICE_TICK_RENEW_TOKEN,
        DEVICE_TICK_POLL,
    } action = DEVICE_TICK_IDLE;

    mock_lock();
    s_test_control_mpq = aosl_mpq_this();
    if (s_block_device_tick) {
        s_test_control_mpq = aosl_mpq_this();
        s_device_tick_entered = true;
        struct timespec deadline;
        assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
        deadline.tv_sec += 5;
        while (s_block_device_tick) {
            assert(pthread_cond_timedwait(&s_io_cond, &s_lock, &deadline) == 0);
        }
    }
    if (s_device_pair_requested && s_device_state == MYBOT_DEVICE_STATE_RUNTIME) {
        s_device_pair_requested = false;
        callbacks = s_device_callbacks;
        action = DEVICE_TICK_PAIR;
    } else if (s_device_stop_requested && s_device_state == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        s_device_stop_requested = false;
        s_device_state = MYBOT_DEVICE_STATE_RUNTIME;
        callbacks = s_device_callbacks;
        action = DEVICE_TICK_STOP;
    } else if (s_device_token_renewal_requested &&
               s_device_state == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        s_device_token_renewal_requested = false;
        callbacks = s_device_callbacks;
        action = DEVICE_TICK_RENEW_TOKEN;
    } else if (s_device_start_requested && s_device_network_available &&
               s_device_state == MYBOT_DEVICE_STATE_RUNTIME) {
        s_device_start_requested = false;
        s_device_state = MYBOT_DEVICE_STATE_IN_CONVERSATION;
        callbacks = s_device_callbacks;
        action = DEVICE_TICK_START;
    } else if (s_service_poll_requested) {
        s_service_poll_requested = false;
        callbacks = s_device_callbacks;
        action = DEVICE_TICK_POLL;
    }
    mock_unlock();

    if (action == DEVICE_TICK_IDLE) {
        return;
    }

    if (action == DEVICE_TICK_POLL) {
        submit_mock_service_request(&callbacks, MYBOT_DEVICE_SERVICE_DEVICE_POLL, NULL);
        return;
    }

    if (action == DEVICE_TICK_PAIR) {
        if (callbacks.on_pair_code) {
            callbacks.on_pair_code("123456", callbacks.user_data);
        }
        if (callbacks.on_state_changed) {
            callbacks.on_state_changed(MYBOT_DEVICE_STATE_AWAITING_CLAIM, callbacks.user_data);
            callbacks.on_state_changed(MYBOT_DEVICE_STATE_RUNTIME, callbacks.user_data);
        }
        mock_lock();
        s_pair_callback_calls++;
        mock_unlock();
        return;
    }

    if (action == DEVICE_TICK_STOP) {
        lifecycle->conversation_id[0] = '\0';
        aosl_atomic_set(&lifecycle->stop_request, 0);
        if (callbacks.on_conversation_stop) {
            callbacks.on_conversation_stop(callbacks.user_data);
        }
        if (callbacks.on_state_changed) {
            callbacks.on_state_changed(MYBOT_DEVICE_STATE_RUNTIME, callbacks.user_data);
        }
        submit_mock_service_request(&callbacks, MYBOT_DEVICE_SERVICE_STOP,
                                    MYBOT_CONVERSATION_STOP_REASON_USER_REQUESTED);
        return;
    }

    if (action == DEVICE_TICK_RENEW_TOKEN) {
        assert(callbacks.on_rtc_token_renewed != NULL);
        assert(callbacks.on_rtc_token_renewed("renewed-token", callbacks.user_data) == 0);
        return;
    }

    if (callbacks.on_state_changed) {
        callbacks.on_state_changed(MYBOT_DEVICE_STATE_IN_CONVERSATION, callbacks.user_data);
    }
    mybot_device_conversation_t params;
    memset(&params, 0, sizeof(params));
    snprintf(params.conversation_id, sizeof(params.conversation_id), "%s", "conversation-1");
    snprintf(params.rtc_app_id, sizeof(params.rtc_app_id), "%s", "rtc-app");
    snprintf(params.rtc_channel, sizeof(params.rtc_channel), "%s", "rtc-channel");
    snprintf(params.rtc_uid, sizeof(params.rtc_uid), "%s", "device-uid");
    snprintf(params.rtc_agent_uid, sizeof(params.rtc_agent_uid), "%s", "agent-uid");
    snprintf(params.rtc_token, sizeof(params.rtc_token), "%s", "rtc-token");
    memcpy(lifecycle->conversation_id, params.conversation_id, sizeof(lifecycle->conversation_id));
    callbacks.on_conversation_start(&params, callbacks.user_data);
}

void mybot_device_lifecycle_set_network_available(mybot_device_lifecycle_t *lifecycle,
                                                  bool available) {
    assert(lifecycle == s_device_lifecycle);
    mock_lock();
    bool changed = s_device_network_available != available;
    s_device_network_available = available;
    s_network_set_calls++;
    if (changed && available) {
        s_network_up_calls++;
    } else if (changed) {
        s_network_down_calls++;
    }
    mock_unlock();
}

void mybot_device_lifecycle_shutdown(mybot_device_lifecycle_t *lifecycle) {
    assert(lifecycle == s_device_lifecycle);
    observe_control_thread(0);
    mybot_device_lifecycle_callbacks_t callbacks;
    bool stop_conversation = false;
    mock_lock();
    s_device_shutdown_calls++;
    lifecycle->conversation_id[0] = '\0';
    aosl_atomic_set(&lifecycle->stop_request, 0);
    if (s_device_state == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        s_device_state = MYBOT_DEVICE_STATE_RUNTIME;
        callbacks = s_device_callbacks;
        stop_conversation = true;
    }
    mock_unlock();
    if (stop_conversation && callbacks.on_conversation_stop) {
        callbacks.on_conversation_stop(callbacks.user_data);
    }
    if (stop_conversation) {
        submit_mock_service_request(&callbacks, MYBOT_DEVICE_SERVICE_STOP,
                                    MYBOT_CONVERSATION_STOP_REASON_DEVICE_HANGUP);
    }
}

void mybot_device_lifecycle_request_pair(mybot_device_lifecycle_t *lifecycle) {
    assert(lifecycle == s_device_lifecycle);
    mock_lock();
    s_pair_requests++;
    if (s_device_state == MYBOT_DEVICE_STATE_RUNTIME) {
        s_device_pair_requested = true;
    }
    mock_unlock();
}

void mybot_device_lifecycle_request_start(mybot_device_lifecycle_t *lifecycle) {
    assert(lifecycle == s_device_lifecycle);
    mock_lock();
    s_device_start_requested = true;
    mock_unlock();
}

void mybot_device_lifecycle_request_stop(mybot_device_lifecycle_t *lifecycle) {
    assert(lifecycle == s_device_lifecycle);
    mock_lock();
    s_stop_requests++;
    s_device_stop_requested = true;
    aosl_atomic_set(&lifecycle->stop_request, 1);
    mock_unlock();
}

void mybot_device_lifecycle_notify_conversation_ended(mybot_device_lifecycle_t *lifecycle) {
    assert(lifecycle == s_device_lifecycle);
    mock_lock();
    if (s_device_state == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        s_device_stop_requested = true;
    }
    s_conversation_ended_notifications++;
    mock_unlock();
}

void mybot_device_lifecycle_request_rtc_token_renewal(mybot_device_lifecycle_t *lifecycle) {
    assert(lifecycle == s_device_lifecycle);
    mock_lock();
    s_token_renewal_requests++;
    s_device_token_renewal_requested = true;
    mock_unlock();
}

static aosl_atomic_t s_rtc_notification_failed;

bool mybot_agora_rtc_take_notification_failure(void) {
    return aosl_atomic_xchg(&s_rtc_notification_failed, false) != 0;
}

int mybot_agora_rtc_init(const char *app_id, const mybot_agora_rtc_callbacks_t *callbacks) {
    if (!app_id || !callbacks) {
        return -1;
    }
    assert(strcmp(app_id, "rtc-app") == 0);
    assert(callbacks != NULL);
    observe_control_thread(CONTROL_OBS_RTC_INIT);
    mock_lock();
    s_rtc_init_calls++;
    int result = s_rtc_init_result;
    if (result < 0) {
        mock_unlock();
        return result;
    }
    bool acquire_aosl_ref = !s_rtc_initialized;
    s_rtc_callbacks = *callbacks;
    s_rtc_initialized = true;
    mock_unlock();
    if (acquire_aosl_ref) {
        /* Mirror the real Agora SDK's independent AOSL ownership. */
        aosl_ctor();
    }
    return 0;
}

int mybot_agora_rtc_join(const char *channel, const char *token, const char *user_account,
                         uint32_t video_min_bps, uint32_t video_max_bps) {
    if (!channel || !token || !user_account) {
        return -1;
    }
#if MYBOT_ENABLE_VIDEO
    assert(video_min_bps == 32000U);
    assert(video_max_bps == 256000U);
#else
    assert(video_min_bps == 0U);
    assert(video_max_bps == 0U);
#endif
    assert(strcmp(token, "rtc-token") == 0);
    observe_control_thread(CONTROL_OBS_RTC_JOIN);
    mock_lock();
    assert(s_rtc_initialized);
    s_rtc_join_calls++;
    int result = s_rtc_join_result;
    if (result < 0) {
        mock_unlock();
        return result;
    }
    snprintf(s_join_channel, sizeof(s_join_channel), "%s", channel);
    snprintf(s_join_user, sizeof(s_join_user), "%s", user_account);
    s_rtc_joined = true;
#if MYBOT_ENABLE_VIDEO
    mock_unlock();
    /* RTC callbacks execute outside the application control worker. */
    emit_rtc_connected();
#else
    void (*callback)(mybot_rtc_state_t, void *) = s_rtc_callbacks.on_state_changed;
    void *user_data = s_rtc_callbacks.user_data;
    mock_unlock();
    if (callback) {
        callback(MYBOT_RTC_STATE_CONNECTED, user_data);
    }
#endif
    return 0;
}

int mybot_agora_rtc_leave(void) {
    observe_control_thread(CONTROL_OBS_RTC_LEAVE);
    mock_lock();
    bool was_joined = s_rtc_joined;
    s_rtc_joined = false;
    if (was_joined) {
        s_rtc_leave_calls++;
    }
    void (*callback)(mybot_rtc_state_t, void *) = s_rtc_callbacks.on_state_changed;
    void *user_data = s_rtc_callbacks.user_data;
    mock_unlock();
    if (was_joined && callback) {
        callback(MYBOT_RTC_STATE_INITIALIZED, user_data);
    }
    return 0;
}

int mybot_agora_rtc_fini(void) {
    observe_control_thread(0);
    mock_lock();
    if (s_rtc_fini_retry) {
        mock_unlock();
        return MYBOT_RTC_FINI_RETRY;
    }
    if (!s_rtc_initialized) {
        mock_unlock();
        return 0;
    }
    assert(!s_rtc_joined);
    s_rtc_initialized = false;
    s_rtc_fini_calls++;
    mock_unlock();
    /* Release the mock SDK's independent AOSL ownership. */
    aosl_dtor();
    return 0;
}

static void *start_thread(void *arg) {
    mybot_config_t *config = arg;
    int result = mybot_start(config);
    mock_lock();
    s_start_thread_result = result;
    s_start_thread_returned = true;
    mock_unlock();
    return NULL;
}

static void *stop_thread(void *arg) {
    (void)arg;
    mock_lock();
    s_stop_thread_entered = true;
    assert(pthread_cond_broadcast(&s_io_cond) == 0);
    mock_unlock();
    mybot_stop();
    mock_lock();
    s_stop_thread_returned = true;
    assert(pthread_cond_broadcast(&s_io_cond) == 0);
    mock_unlock();
    return NULL;
}

int mybot_agora_rtc_send_audio(const void *data, size_t len) {
    mock_lock();
    if (!s_rtc_joined) {
        mock_unlock();
        return -1;
    }
    int index = s_rtc_send_calls;
    if (index < TEST_MAX_SENT_FRAMES) {
        s_sent_lengths[index] = len;
        size_t copy_len = len < TEST_SEND_FRAME_BYTES ? len : TEST_SEND_FRAME_BYTES;
        memcpy(s_sent_frames[index], data, copy_len);
    }
    s_rtc_send_calls++;
    mock_unlock();
    return 0;
}

#if MYBOT_ENABLE_VIDEO
int mybot_agora_rtc_send_video(const mybot_video_frame_t *frame) {
    if (!frame || !frame->data || frame->len == 0) {
        return -1;
    }
    return 0;
}
#endif

static int record_renewed_token(const char *token) {
    mock_lock();
    if (!s_rtc_joined || !token || !token[0]) {
        mock_unlock();
        return -1;
    }
    snprintf(s_renewed_token, sizeof(s_renewed_token), "%s", token);
    s_rtc_renew_calls++;
    mock_unlock();
    return 0;
}

static void renew_token_on_rtc(const aosl_ts_t *queued_ts, aosl_refobj_t robj, uintptr_t argc,
                               uintptr_t argv[]) {
    (void)queued_ts;
    (void)robj;
    assert(argc == 2);
    assert(aosl_mpq_this() == s_test_rtc_mpq);
    *(int *)argv[1] = record_renewed_token((const char *)argv[0]);
}

int mybot_agora_rtc_renew_token(const char *token) {
    observe_control_thread(CONTROL_OBS_RTC_RENEW);
    if (aosl_mpq_invalid(s_test_rtc_mpq)) {
        return record_renewed_token(token);
    }
    int result = -1;
    if (aosl_mpq_call(s_test_rtc_mpq, AOSL_REF_INVALID, "renew_token_on_rtc", renew_token_on_rtc, 2,
                      (uintptr_t)token, (uintptr_t)&result) < 0) {
        return -1;
    }
    return result;
}

typedef enum {
    RTC_FLOOD_LCD,
    RTC_FLOOD_KEY,
    RTC_FLOOD_WIFI,
#if MYBOT_ENABLE_VIDEO
    RTC_FLOOD_VIDEO_START,
    RTC_FLOOD_VIDEO_OPTIONAL,
#endif
} rtc_flood_t;

static void flood_control_from_rtc(const aosl_ts_t *queued_ts, aosl_refobj_t robj, uintptr_t argc,
                                   uintptr_t argv[]) {
    (void)queued_ts;
    (void)robj;
    assert(argc == 1);
    assert(aosl_mpq_this() == s_test_rtc_mpq);
    const char message[] = "{\"event_type\":\"state.listening\",\"payload\":{\"value\":true}}";
    /* Control is paused in simulated service I/O, so the 1001st callback must
     * return even though the 1000-event queue cannot consume any events. */
    for (int i = 0; i < 1001; i++) {
        emit_rtm_subscribe_data("rtc-channel", "agent-uid", message, sizeof(message) - 1U);
    }
    switch ((rtc_flood_t)argv[0]) {
    case RTC_FLOOD_LCD:
        break;
    case RTC_FLOOD_KEY:
        emit_key_event(MYBOT_KEY_EVENT_CONVERSATION_STOP);
        break;
    case RTC_FLOOD_WIFI:
        emit_wifi_event(MYBOT_WIFI_EVENT_STA_DISCONNECTED);
        break;
#if MYBOT_ENABLE_VIDEO
    case RTC_FLOOD_VIDEO_START:
        s_rtc_callbacks.on_state_changed(MYBOT_RTC_STATE_CONNECTED, s_rtc_callbacks.user_data);
        break;
    case RTC_FLOOD_VIDEO_OPTIONAL:
        s_rtc_callbacks.on_video_key_frame_requested(s_rtc_callbacks.user_data);
        s_rtc_callbacks.on_video_target_bitrate_changed(96000U, s_rtc_callbacks.user_data);
        break;
#endif
    }
    mock_lock();
    s_rtc_flood_returned = true;
    mock_unlock();
}

static void pause_control_for_queue_test(const mybot_config_t *config) {
    assert(mybot_start(config) == 0);
    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
#if MYBOT_ENABLE_VIDEO
    intptr_t video_starts_before = aosl_atomic_read(&s_video_start_calls);
#endif
    int joins_before = read_counter(&s_rtc_join_calls);
    emit_key_event(MYBOT_KEY_EVENT_CONVERSATION_START);
    assert(wait_for_counter(&s_rtc_join_calls, joins_before + 1, 1000));
    assert(wait_for_app_state(MYBOT_STATE_IN_CONVERSATION, 1000));
#if MYBOT_ENABLE_VIDEO
    assert(wait_for_video_counter(&s_video_start_calls, video_starts_before + 1));
#endif
    mock_lock();
    s_block_device_tick = true;
    s_device_tick_entered = false;
    s_rtc_flood_returned = false;
    mock_unlock();
    assert(wait_for_flag(&s_device_tick_entered, 1000));
}

static void resume_control_after_queue_test(void) {
    mock_lock();
    s_block_device_tick = false;
    assert(pthread_cond_broadcast(&s_io_cond) == 0);
    mock_unlock();
}

static void saturate_control_queue(rtc_flood_t event) {
    assert(aosl_mpq_queue(s_test_rtc_mpq, AOSL_MPQ_INVALID, AOSL_REF_INVALID,
                          "flood_control_from_rtc", flood_control_from_rtc, 1,
                          (uintptr_t)event) == 0);
    assert(wait_for_flag(&s_rtc_flood_returned, 1000));
}

static void wait_for_control_queue_empty(void) {
    for (int elapsed = 0; elapsed < 1000; elapsed++) {
        if (aosl_mpq_queued_count(s_test_control_mpq) == 0) {
            return;
        }
        aosl_hal_msleep(1);
    }
    assert(!"control queue did not drain");
}

static void test_control_queue_saturation(const mybot_config_t *config) {
    int previous_log_level = aosl_get_log_level();
    aosl_set_log_level(AOSL_LOG_NOTICE);
    /* Own an AOSL reference while the test RTC queue outlives each SDK run. */
    aosl_ctor();
    s_test_rtc_mpq = aosl_mpq_create_flags(AOSL_MPQ_FLAG_SIGP_EVENT | AOSL_MPQ_FLAG_NONBLOCK,
                                           AOSL_THRD_PRI_NORMAL, 64 * 1024, 16, "test_rtc_mpq",
                                           NULL, NULL, NULL);
    assert(!aosl_mpq_invalid(s_test_rtc_mpq));

    pause_control_for_queue_test(config);
    int renewals_before = read_counter(&s_rtc_renew_calls);
    s_rtc_callbacks.on_token_will_expire(s_rtc_callbacks.user_data);
    saturate_control_queue(RTC_FLOOD_LCD);
    assert(mybot_is_running());
    assert(read_counter(&s_rtc_renew_calls) == renewals_before);
    resume_control_after_queue_test();
    /* The resumed control worker synchronously calls the same RTC queue whose
     * callbacks previously saturated control; both workers must progress. */
    assert(wait_for_counter(&s_rtc_renew_calls, renewals_before + 1, 1000));
    assert(wait_for_lcd_indicator(MYBOT_LCD_INDICATOR_LISTENING, 1000));
    assert(mybot_is_running());
    wait_for_control_queue_empty();
    mybot_stop();
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);

    pause_control_for_queue_test(config);
    saturate_control_queue(RTC_FLOOD_LCD);
    mock_lock();
    s_stop_thread_entered = false;
    s_stop_thread_returned = false;
    int shutdowns_before = s_device_shutdown_calls;
    int leaves_before = s_rtc_leave_calls;
    int rtc_finis_before = s_rtc_fini_calls;
    int captures_before = s_capture_destroy_calls;
    int playbacks_before = s_playback_destroy_calls;
    mock_unlock();
    pthread_t stop_tid;
    int thread_result = pthread_create(&stop_tid, NULL, stop_thread, NULL);
    assert(thread_result == 0);
    assert(wait_for_flag(&s_stop_thread_entered, 1000));
    for (int elapsed = 0; elapsed < 1000 && mybot_is_running(); elapsed++) {
        aosl_hal_msleep(1);
    }
    assert(!mybot_is_running());
    assert(!read_bool(&s_stop_thread_returned));
    assert(read_counter(&s_device_shutdown_calls) == shutdowns_before);
    resume_control_after_queue_test();
    assert(wait_for_flag(&s_stop_thread_returned, 1000));
    assert(pthread_join(stop_tid, NULL) == 0);
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);
    assert(read_counter(&s_device_shutdown_calls) == shutdowns_before + 1);
    assert(read_counter(&s_rtc_leave_calls) == leaves_before + 1);
    assert(read_counter(&s_rtc_fini_calls) == rtc_finis_before + 1);
    assert(read_counter(&s_capture_destroy_calls) == captures_before + 1);
    assert(read_counter(&s_playback_destroy_calls) == playbacks_before + 1);
    mybot_stop();
    assert(read_counter(&s_device_shutdown_calls) == shutdowns_before + 1);

    const rtc_flood_t critical_events[] = {
        RTC_FLOOD_KEY,
        RTC_FLOOD_WIFI,
#if MYBOT_ENABLE_VIDEO
        RTC_FLOOD_VIDEO_START,
#endif
    };
    for (size_t i = 0; i < sizeof(critical_events) / sizeof(critical_events[0]); i++) {
        pause_control_for_queue_test(config);
        saturate_control_queue(critical_events[i]);
        assert(!mybot_is_running());
        resume_control_after_queue_test();
        mybot_stop();
        assert(mybot_get_state() == MYBOT_STATE_STOPPED);
    }

#if MYBOT_ENABLE_VIDEO
    pause_control_for_queue_test(config);
    intptr_t key_frames_before = aosl_atomic_read(&s_video_key_frame_calls);
    intptr_t bitrates_before = aosl_atomic_read(&s_video_bitrate_calls);
    saturate_control_queue(RTC_FLOOD_VIDEO_OPTIONAL);
    assert(mybot_is_running());
    resume_control_after_queue_test();
    assert(wait_for_lcd_indicator(MYBOT_LCD_INDICATOR_LISTENING, 1000));
    wait_for_control_queue_empty();
    wait_for_control_events();
    assert(aosl_atomic_read(&s_video_key_frame_calls) == key_frames_before);
    assert(aosl_atomic_read(&s_video_bitrate_calls) == bitrates_before);
    mybot_stop();
#endif

    assert(mybot_start(config) == 0);
    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
    mybot_stop();
    assert(aosl_mpq_destroy_wait(s_test_rtc_mpq) == 0);
    s_test_rtc_mpq = AOSL_MPQ_INVALID;
    aosl_dtor();
    aosl_set_log_level(previous_log_level);
}

static void test_service_startup_failures(const mybot_config_t *config) {
    s_lcd_init_fails = true;
    int wifi_before = read_counter(&s_wifi_init_calls);
    assert(mybot_start(config) < 0);
    assert(!mybot_is_running());
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);
    assert(read_counter(&s_wifi_init_calls) == wifi_before);
    s_lcd_init_fails = false;

    assert(mybot_start(config) == 0);
    emit_wifi_event(MYBOT_WIFI_EVENT_FAILED);
    assert(wait_for_app_state(MYBOT_STATE_FAILED, 1000));
    assert(wait_for_runtime_exit(1000));
    mybot_stop();
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);

    for (int failure = SERVICE_FAIL_KEY_INIT; failure <= SERVICE_FAIL_DEVICE_INIT; ++failure) {
        int capture_destroys = read_counter(&s_capture_destroy_calls);
        int playback_destroys = read_counter(&s_playback_destroy_calls);
        int device_shutdowns = read_counter(&s_device_shutdown_calls);
        mock_lock();
        s_service_failure = failure;
        mock_unlock();
        assert(mybot_start(config) == 0);
        emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
        assert(wait_for_app_state(MYBOT_STATE_FAILED, 1000));
        assert(wait_for_runtime_exit(1000));
        mybot_stop();
        assert(mybot_get_state() == MYBOT_STATE_STOPPED);
        assert(read_counter(&s_capture_destroy_calls) ==
               capture_destroys + (failure >= SERVICE_FAIL_CAPTURE_START));
        assert(read_counter(&s_playback_destroy_calls) ==
               playback_destroys + (failure == SERVICE_FAIL_CAPTURE_START ||
                                    failure >= SERVICE_FAIL_PLAYBACK_START));
        assert(read_counter(&s_device_shutdown_calls) == device_shutdowns);

        /* Every partial-start failure must leave a complete restart possible. */
        mock_lock();
        s_service_failure = SERVICE_FAIL_NONE;
        mock_unlock();
        assert(mybot_start(config) == 0);
        emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
        assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
        mybot_stop();
        assert(mybot_get_state() == MYBOT_STATE_STOPPED);
    }
}

static void service_barrier(const aosl_ts_t *ts, aosl_refobj_t ref, uintptr_t argc,
                            uintptr_t argv[]) {
    (void)ts;
    (void)ref;
    (void)argv;
    assert(argc == 0);
}

static void drain_service_requests(void) {
    mock_lock();
    aosl_mpq_t service = s_test_service_mpq;
    mock_unlock();
    assert(!aosl_mpq_invalid(service));
    assert(aosl_mpq_call(service, AOSL_REF_INVALID, "service_test_barrier", service_barrier, 0) ==
           0);
    drain_device_commands();
}

static void test_background_service_requests(const mybot_config_t *config) {
    begin_control_thread_tracking();
    mock_lock();
    s_track_service_thread = true;
    s_service_thread_valid = false;
    mock_unlock();
    assert(mybot_start(config) == 0);
    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));

    /* A status request remains blocked while both start and local hangup run. */
    mock_lock();
    s_block_service_poll = true;
    s_service_poll_entered = false;
    s_service_poll_requested = true;
    int polls_before = s_service_poll_calls;
    int results_before = s_service_result_calls;
    int joins_before = s_rtc_join_calls;
    int leaves_before = s_rtc_leave_calls;
    mock_unlock();
    wait_for_service_flag(&s_service_poll_entered);
    emit_key_event(MYBOT_KEY_EVENT_CONVERSATION_START);
    assert(wait_for_counter(&s_rtc_join_calls, joins_before + 1, 1000));
    assert(wait_for_app_state(MYBOT_STATE_IN_CONVERSATION, 1000));
    emit_key_event(MYBOT_KEY_EVENT_CONVERSATION_STOP);
    assert(wait_for_counter(&s_rtc_leave_calls, leaves_before + 1, 1000));
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
    assert(read_counter(&s_service_poll_calls) == polls_before);
    assert(read_counter(&s_service_result_calls) == results_before);
    mock_lock();
    s_block_service_poll = false;
    assert(pthread_cond_broadcast(&s_io_cond) == 0);
    mock_unlock();
    drain_service_requests();
    assert(read_counter(&s_service_poll_calls) == polls_before + 1);
    assert(read_counter(&s_service_result_calls) >= results_before + 1);

    /* A slow stop POST must not hold up the next conversation's start. */
    emit_key_event(MYBOT_KEY_EVENT_CONVERSATION_START);
    assert(wait_for_counter(&s_rtc_join_calls, joins_before + 2, 1000));
    assert(wait_for_app_state(MYBOT_STATE_IN_CONVERSATION, 1000));
    mock_lock();
    s_block_service_stop = true;
    s_service_stop_result = 503;
    s_service_stop_entered = false;
    int stops_before = s_service_stop_calls;
    mock_unlock();
    emit_key_event(MYBOT_KEY_EVENT_CONVERSATION_STOP);
    assert(wait_for_counter(&s_rtc_leave_calls, leaves_before + 2, 1000));
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
    wait_for_service_flag(&s_service_stop_entered);
    emit_key_event(MYBOT_KEY_EVENT_CONVERSATION_START);
    assert(wait_for_counter(&s_rtc_join_calls, joins_before + 3, 1000));
    assert(wait_for_app_state(MYBOT_STATE_IN_CONVERSATION, 1000));
    assert(read_counter(&s_service_stop_calls) == stops_before);
    mock_lock();
    s_block_service_stop = false;
    assert(pthread_cond_broadcast(&s_io_cond) == 0);
    mock_unlock();
    drain_service_requests();
    assert(read_counter(&s_service_stop_calls) == stops_before + 1);
    mock_lock();
    s_service_stop_result = 0;
    mock_unlock();

    /* Shutdown waits for HTTP ownership, discards its result, and still sends
     * the final stop notification queued behind the in-flight poll. */
    mock_lock();
    s_block_service_poll = true;
    s_service_poll_entered = false;
    s_service_poll_requested = true;
    s_stop_thread_entered = false;
    s_stop_thread_returned = false;
    results_before = s_service_result_calls;
    stops_before = s_service_stop_calls;
    int shutdowns_before = s_device_shutdown_calls;
#if MYBOT_TEST_WRAP_APP_MPQ
    int service_destroys_before = s_service_destroy_calls;
#endif
    mock_unlock();
    wait_for_service_flag(&s_service_poll_entered);
    pthread_t stop_tid;
    int thread_result = pthread_create(&stop_tid, NULL, stop_thread, NULL);
    assert(thread_result == 0);
    wait_for_service_flag(&s_stop_thread_entered);
    assert(wait_for_counter(&s_device_shutdown_calls, shutdowns_before + 1, 1000));
#if MYBOT_TEST_WRAP_APP_MPQ
    assert(wait_for_counter(&s_service_destroy_calls, service_destroys_before + 1, 1000));
#endif
    assert(!mybot_is_running());
    assert(!read_bool(&s_stop_thread_returned));
    assert(read_counter(&s_service_result_calls) == results_before);
    mock_lock();
    s_block_service_poll = false;
    assert(pthread_cond_broadcast(&s_io_cond) == 0);
    mock_unlock();
    wait_for_service_flag(&s_stop_thread_returned);
    thread_result = pthread_join(stop_tid, NULL);
    assert(thread_result == 0);
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);
    assert(read_counter(&s_service_stop_calls) == stops_before + 1);
    assert(read_counter(&s_service_result_calls) == results_before);
    mock_lock();
    assert(s_control_thread_valid && !s_control_thread_mismatch);
    assert(s_service_thread_valid);
    s_track_control_thread = false;
    s_track_service_thread = false;
    mock_unlock();
}

#if MYBOT_TEST_WRAP_APP_MPQ
static void test_media_join_retries(const mybot_config_t *config) {
    const char *queues[] = {"cap_mpq", "pb_mpq", "mybot_mpq"};
    for (size_t i = 0; i < sizeof(queues) / sizeof(queues[0]); ++i) {
        int captures = read_counter(&s_capture_destroy_calls);
        int playbacks = read_counter(&s_playback_destroy_calls);
        s_media_destroy_failure = queues[i];
        assert(mybot_start(config) == 0);
        emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
        assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
        assert(!aosl_mpq_invalid(s_media_unjoinable));

        mybot_stop();
        assert(!mybot_is_running());
        assert(mybot_get_state() == MYBOT_STATE_STOPPING);
        assert(read_counter(&s_capture_destroy_calls) == captures);
        assert(read_counter(&s_playback_destroy_calls) == playbacks);
        assert(mybot_start(config) < 0);

        s_media_unjoinable = AOSL_MPQ_INVALID;
        s_media_destroy_failure = NULL;
        mybot_stop();
        assert(mybot_get_state() == MYBOT_STATE_STOPPED);
        assert(read_counter(&s_capture_destroy_calls) == captures + 1);
        assert(read_counter(&s_playback_destroy_calls) == playbacks + 1);
    }

    assert(mybot_start(config) == 0);
    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
    emit_key_event(MYBOT_KEY_EVENT_CONVERSATION_START);
    assert(wait_for_app_state(MYBOT_STATE_IN_CONVERSATION, 1000));
    mock_lock();
    s_rtc_fini_retry = true;
    mock_unlock();
    mybot_stop();
    assert(mybot_get_state() == MYBOT_STATE_STOPPING);
    assert(mybot_start(config) < 0);
    mock_lock();
    s_rtc_fini_retry = false;
    mock_unlock();
    mybot_stop();
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);
}

static void test_control_queue_failures(const mybot_config_t *config) {
    int wifi_before = read_counter(&s_wifi_init_calls);
    s_control_create_fails = true;
    assert(mybot_start(config) < 0);
    assert(!mybot_is_running());
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);
    assert(read_counter(&s_wifi_init_calls) == wifi_before);
    s_control_create_fails = false;

    s_service_create_fails = true;
    assert(mybot_start(config) == 0);
    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_FAILED, 1000));
    assert(wait_for_runtime_exit(1000));
    mybot_stop();
    assert(!mybot_is_running());
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);
    s_service_create_fails = false;
    wifi_before = read_counter(&s_wifi_init_calls);

    s_control_call_fails = true;
    assert(mybot_start(config) < 0);
    assert(!mybot_is_running());
    assert(read_counter(&s_wifi_init_calls) == wifi_before);
    s_control_call_fails = false;

    const char *media_queues[] = {"cap_mpq", "pb_mpq", "mybot_mpq"};
    for (size_t i = 0; i < sizeof(media_queues) / sizeof(media_queues[0]); ++i) {
        int capture_destroys = read_counter(&s_capture_destroy_calls);
        int playback_destroys = read_counter(&s_playback_destroy_calls);
        s_media_create_failure = media_queues[i];
        assert(mybot_start(config) == 0);
        emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
        assert(wait_for_app_state(MYBOT_STATE_FAILED, 1000));
        assert(wait_for_runtime_exit(1000));
        mybot_stop();
        assert(read_counter(&s_capture_destroy_calls) == capture_destroys + 1);
        assert(read_counter(&s_playback_destroy_calls) == playback_destroys + 1);
        assert(mybot_get_state() == MYBOT_STATE_STOPPED);
        s_media_create_failure = NULL;
    }

    assert(mybot_start(config) == 0);
    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
    int destroys_before = read_counter(&s_capture_destroy_calls);
    s_control_destroy_fails = true;
    mybot_stop();
    assert(!mybot_is_running());
    assert(s_control_destroy_failures == 1);
    assert(mybot_get_state() == MYBOT_STATE_STOPPING);
    assert(read_counter(&s_capture_destroy_calls) == destroys_before + 1);
    assert(mybot_start(config) < 0);
    s_control_destroy_fails = false;
    mybot_stop();
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);
    assert(read_counter(&s_capture_destroy_calls) == destroys_before + 1);

    /* Startup cleanup also retains queue ownership when its first join fails. */
    s_control_call_fails = true;
    s_control_destroy_fails = true;
    assert(mybot_start(config) < 0);
    assert(s_control_destroy_failures == 2);
    assert(mybot_get_state() == MYBOT_STATE_STOPPING);
    assert(mybot_start(config) < 0);
    s_control_call_fails = false;
    s_control_destroy_fails = false;
    mybot_stop();
    assert(mybot_start(config) == 0);
    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
    mybot_stop();
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);

    /* A failed HTTP-worker join keeps its runtime reference until a later stop
     * can drain it; it must also prevent a new start from reusing the runtime. */
    assert(mybot_start(config) == 0);
    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
    mock_lock();
    s_service_destroy_fails = true;
    int service_failures_before = s_service_destroy_failures;
    mock_unlock();
    mybot_stop();
    assert(!mybot_is_running());
    assert(mybot_get_state() == MYBOT_STATE_STOPPING);
    assert(read_counter(&s_service_destroy_failures) > service_failures_before);
    assert(mybot_start(config) < 0);
    mock_lock();
    s_service_destroy_fails = false;
    mock_unlock();
    mybot_stop();
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);
    assert(mybot_start(config) == 0);
    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
    mybot_stop();
}
#endif

static size_t s_envelope_alloc_calls;
static size_t s_envelope_alloc_fail_at;
static size_t s_envelope_alloc_live;

static void *envelope_json_malloc(size_t size) {
    if (++s_envelope_alloc_calls == s_envelope_alloc_fail_at) {
        return NULL;
    }
    void *pointer = aosl_hal_malloc(size);
    if (pointer) {
        ++s_envelope_alloc_live;
    }
    return pointer;
}

static void envelope_json_free(void *pointer) {
    if (pointer) {
        assert(s_envelope_alloc_live > 0);
        --s_envelope_alloc_live;
    }
    aosl_hal_free(pointer);
}

static void test_cloud_message_allocation_failures(const char *message, size_t length) {
    /* This test's RTM parsing runs synchronously on the caller. No JSON trees
     * remain live while the allocator hooks are changed. */
    mybot_json_hooks_t hooks = {.malloc_fn = envelope_json_malloc, .free_fn = envelope_json_free};
    s_envelope_alloc_calls = 0;
    s_envelope_alloc_fail_at = 0;
    assert(mybot_json_init_hooks(&hooks) == 0);
    mybot_json_t *outer = mybot_json_parse(message);
    assert(outer != NULL);
    size_t outer_allocations = s_envelope_alloc_calls;
    mybot_json_delete(outer);
    assert(s_envelope_alloc_live == 0);

    int sets_before = read_counter(&s_media_volume_set_calls);
    const size_t failures[] = {1, outer_allocations + 1};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        s_envelope_alloc_calls = 0;
        s_envelope_alloc_fail_at = failures[i];
        emit_rtm_data("agent-uid", message, length);
        assert(s_envelope_alloc_calls == failures[i]);
        assert(s_envelope_alloc_live == 0);
        drain_device_commands();
        assert(read_counter(&s_media_volume_set_calls) == sets_before);
    }
    assert(mybot_json_init_hooks(NULL) == 0);
}

static size_t make_cloud_message(char *message, size_t capacity, const char *fields,
                                 const char *content_key, const char *content) {
    int prefix = snprintf(message, capacity, "{%s,\"%s\":\"", fields, content_key);
    assert(prefix > 0 && (size_t)prefix < capacity);
    size_t length = (size_t)prefix;
    for (const char *p = content; *p; ++p) {
        assert((unsigned char)*p >= 0x20);
        assert(length + 4 < capacity);
        if (*p == '\\' || *p == '"') {
            message[length++] = '\\';
        }
        message[length++] = *p;
    }
    message[length++] = '"';
    message[length++] = '}';
    message[length] = '\0';
    return length;
}

static void emit_cloud_message(const char *channel, const char *fields, const char *content_key,
                               const char *content) {
    char message[1024];
    size_t length = make_cloud_message(message, sizeof(message), fields, content_key, content);
    if (channel) {
        emit_rtm_subscribe_data(channel, "agent-uid", message, length);
    } else {
        emit_rtm_data("agent-uid", message, length);
    }
}

static void test_cloud_message_envelope(void) {
    /* The exact RTM payload captured from the device log, including its spacing. */
    static const char captured[] =
        "{\"message_id\": \"ace5db6a\", \"object\": \"message.user\", \"content\": "
        "\"{\\\"type\\\":\\\"device_command\\\",\\\"schema\\\":\\\"botstation.device-command\\\","
        "\\\"version\\\":1,\\\"message_id\\\":\\\"msg_2qGHNLswfXeT-vrRdbieZw\\\","
        "\\\"command_id\\\":\\\"cmd_oeNNZ1uRv7vDgtrnpu6e5A\\\","
        "\\\"conversation_id\\\":\\\"conv_59ld9dYHhZfF\\\","
        "\\\"issued_at\\\":\\\"2026-10-17T08:36:24.039Z\\\","
        "\\\"expires_at\\\":\\\"2026-10-17T08:36:34.039Z\\\","
        "\\\"command\\\":{\\\"type\\\":\\\"action\\\",\\\"name\\\":\\\"audio.set_volume\\\","
        "\\\"params\\\":{\\\"level\\\":50}}}\", \"data_type\": \"message\"}";
    assert(sizeof(captured) - 1U == 486);
    int volume_calls_before = read_counter(&s_media_volume_set_calls);
    emit_rtm_subscribe_data("rtc-channel", "agent-uid", captured, sizeof(captured) - 1U);
    drain_device_commands();
    assert(read_counter(&s_media_volume_set_calls) == volume_calls_before);

    /* Only the captured conversation ID changes to match the live stub session. */
    const char *conversation = strstr(captured, "conv_59ld9dYHhZfF");
    assert(conversation != NULL);
    char matching[sizeof(captured)];
    int length =
        snprintf(matching, sizeof(matching), "%.*sconversation-1%s", (int)(conversation - captured),
                 captured, conversation + strlen("conv_59ld9dYHhZfF"));
    assert(length > 0 && (size_t)length < sizeof(matching));
    emit_rtm_subscribe_data("rtc-channel", "other-uid", matching, (size_t)length);
    emit_rtm_subscribe_data("bad-channel", "agent-uid", matching, (size_t)length);
    emit_rtm_data("other-uid", matching, (size_t)length);
    /* A legal inner command is accepted only inside the actual cloud envelope. */
    emit_bare_device_command(NULL, "agent-uid", "cmd_oeNNZ1uRv7vDgtrnpu6e5A", "conversation-1",
                             s_volume_command);
    emit_bare_device_command("rtc-channel", "agent-uid", "cmd_oeNNZ1uRv7vDgtrnpu6e5A",
                             "conversation-1", s_volume_command);
    drain_device_commands();
    assert(read_counter(&s_media_volume_set_calls) == volume_calls_before);
    test_cloud_message_allocation_failures(matching, (size_t)length);
    emit_rtm_subscribe_data("rtc-channel", "agent-uid", matching, (size_t)length);
    drain_device_commands();
    assert(read_counter(&s_media_volume_set_calls) == volume_calls_before + 1);
    assert(read_counter(&s_last_media_volume) == 50);

    /* Both RTM delivery routes share the inner command ID cache. */
    emit_rtm_data("agent-uid", matching, (size_t)length);
    emit_rtm_subscribe_data("rtc-channel", "agent-uid", matching, (size_t)length);
    emit_device_command(
        NULL, "agent-uid", "cmd_oeNNZ1uRv7vDgtrnpu6e5A", "conversation-1",
        "{\"type\":\"action\",\"name\":\"audio.set_volume\",\"params\":{\"level\":50}}", NULL);
    drain_device_commands();
    assert(read_counter(&s_media_volume_set_calls) == volume_calls_before + 1);

    static const char fields[] =
        "\"message_id\":\"ace5db6a\",\"object\":\"message.user\",\"data_type\":\"message\"";
    static const char inner[] =
        "{\"type\":\"device_command\",\"schema\":\"botstation.device-command\",\"version\":1,"
        "\"message_id\":\"msg-envelope-after-invalid\","
        "\"command_id\":\"cmd-envelope-after-invalid\",\"conversation_id\":\"conversation-1\","
        "\"command\":{\"type\":\"action\",\"name\":\"audio.set_volume\",\"params\":{\"level\":51}}"
        "}";
    static const char *const invalid_fields[] = {
        "\"object\":\"message.assistant\",\"data_type\":\"message\"",
        "\"object\":\"message.user\",\"data_type\":\"text\"",
        "\"object\":\"message.user\"",
        "\"data_type\":\"message\"",
        "\"Object\":\"message.user\",\"data_type\":\"message\"",
        "\"object\":\"message.user\",\"Data_type\":\"message\"",
        "\"object\":null,\"data_type\":\"message\"",
        "\"object\":\"message.user\",\"data_type\":1",
        "\"object\":\"message.user\",\"object\":\"message.user\",\"data_type\":\"message\"",
        "\"object\":\"message.user\",\"data_type\":\"message\",\"data_type\":\"message\"",
        "\"object\":\"message.user\",\"data_type\":\"message\",\"content\":\"ignored\"",
        "\"object\":\"message.user\\u0000suffix\",\"data_type\":\"message\"",
    };
    for (size_t i = 0; i < sizeof(invalid_fields) / sizeof(invalid_fields[0]); ++i) {
        emit_cloud_message("rtc-channel", invalid_fields[i], "content", inner);
    }
    emit_cloud_message(NULL, fields, "Content", inner);
    static const char *const invalid_content[] = {
        "",         "{",
        "[]",       "null",
        "true",     "51",
        "\"text\"", "{\"event_type\":\"state.listening\",\"payload\":{\"value\":true}}",
    };
    for (size_t i = 0; i < sizeof(invalid_content) / sizeof(invalid_content[0]); ++i) {
        emit_cloud_message(NULL, fields, "content", invalid_content[i]);
    }
    char nested[1024];
    make_cloud_message(nested, sizeof(nested), fields, "content", inner);
    emit_cloud_message("rtc-channel", fields, "content", nested);
    static const char *const wrong_content_types[] = {
        "{\"object\":\"message.user\",\"data_type\":\"message\"}",
        "{\"object\":\"message.user\",\"data_type\":\"message\",\"content\":null}",
        "{\"object\":\"message.user\",\"data_type\":\"message\",\"content\":51}",
        "{\"object\":\"message.user\",\"data_type\":\"message\",\"content\":{}}",
    };
    for (size_t i = 0; i < sizeof(wrong_content_types) / sizeof(wrong_content_types[0]); ++i) {
        emit_rtm_data("agent-uid", wrong_content_types[i], strlen(wrong_content_types[i]));
    }
    /* The outer string decodes to an escaped NUL in the inner JSON. Reject
     * that decoded escape and leave the legitimate command ID available. */
    static const char inner_nul[] =
        "{\"type\":\"device_command\",\"schema\":\"botstation.device-command\",\"version\":1,"
        "\"message_id\":\"msg-envelope-after-invalid\","
        "\"command_id\":\"cmd-envelope-after-invalid\\u0000suffix\","
        "\"conversation_id\":\"conversation-1\","
        "\"command\":{\"type\":\"action\",\"name\":\"audio.set_volume\",\"params\":{\"level\":51}}"
        "}";
    emit_cloud_message(NULL, fields, "content", inner_nul);
    static const char duplicate_command[] =
        "{\"type\":\"device_command\",\"schema\":\"botstation.device-command\",\"version\":1,"
        "\"message_id\":\"msg-envelope-after-invalid\","
        "\"command_id\":\"cmd-envelope-after-invalid\",\"conversation_id\":\"conversation-1\","
        "\"command\":{\"type\":\"action\",\"name\":\"audio.set_volume\",\"params\":{\"level\":51}},"
        "\"command\":{\"type\":\"action\",\"name\":\"audio.set_volume\",\"params\":{\"level\":51}}"
        "}";
    emit_cloud_message("rtc-channel", fields, "content", duplicate_command);
    static const char uppercase_inner_key[] =
        "{\"Type\":\"device_command\",\"schema\":\"botstation.device-command\",\"version\":1,"
        "\"message_id\":\"msg-envelope-after-invalid\","
        "\"command_id\":\"cmd-envelope-after-invalid\",\"conversation_id\":\"conversation-1\","
        "\"command\":{\"type\":\"action\",\"name\":\"audio.set_volume\",\"params\":{\"level\":51}}"
        "}";
    emit_cloud_message(NULL, fields, "content", uppercase_inner_key);
    drain_device_commands();
    assert(mybot_get_state() == MYBOT_STATE_IN_CONVERSATION && mybot_is_running());
    assert(read_counter(&s_media_volume_set_calls) == volume_calls_before + 1);
    assert(read_counter(&s_last_media_volume) == 50);
    mock_lock();
    assert(!(s_last_lcd_content.indicators & MYBOT_LCD_INDICATOR_LISTENING));
    mock_unlock();

    /* Rejections do not consume the inner ID or the outer metadata message ID. */
    emit_cloud_message(NULL, fields, "content", inner);
    drain_device_commands();
    assert(read_counter(&s_media_volume_set_calls) == volume_calls_before + 2);
    assert(read_counter(&s_last_media_volume) == 51);
    emit_cloud_message("rtc-channel", fields, "content", inner);
    drain_device_commands();
    assert(read_counter(&s_media_volume_set_calls) == volume_calls_before + 2);

    /* The transport bound counts the entire encoded envelope, including metadata. */
    static const char boundary_inner[] =
        "{\"type\":\"device_command\",\"schema\":\"botstation.device-command\",\"version\":1,"
        "\"message_id\":\"msg-envelope-boundary\",\"command_id\":\"cmd-envelope-boundary\","
        "\"conversation_id\":\"conversation-1\","
        "\"command\":{\"type\":\"action\",\"name\":\"audio.set_volume\",\"params\":{\"level\":52}}"
        "}";
    char boundary[1025];
    size_t boundary_length =
        make_cloud_message(boundary, sizeof(boundary), fields, "content", boundary_inner);
    memset(boundary + boundary_length, ' ', sizeof(boundary) - boundary_length);
    emit_rtm_data("agent-uid", boundary, sizeof(boundary));
    drain_device_commands();
    assert(read_counter(&s_media_volume_set_calls) == volume_calls_before + 2);
    emit_rtm_subscribe_data("rtc-channel", "agent-uid", boundary, sizeof(boundary) - 1U);
    drain_device_commands();
    assert(read_counter(&s_media_volume_set_calls) == volume_calls_before + 3);
    assert(read_counter(&s_last_media_volume) == 52);
}

static void test_cloud_adjust_volume(void) {
    static const char captured[] =
        "{\"message_id\": \"c14413c1\", \"object\": \"message.user\", \"content\": "
        "\"{\\\"type\\\":\\\"device_command\\\",\\\"schema\\\":\\\"botstation.device-command\\\","
        "\\\"version\\\":1,\\\"message_id\\\":\\\"msg_w3fLewic2AFydS6Glp0KpA\\\","
        "\\\"command_id\\\":\\\"cmd_Wr_3v_X0o95dtLOCwwNlcQ\\\","
        "\\\"conversation_id\\\":\\\"conv_dMWMDV-aC2tZ\\\","
        "\\\"issued_at\\\":\\\"2026-10-17T10:00:59.972Z\\\","
        "\\\"expires_at\\\":\\\"2026-10-17T10:01:09.972Z\\\","
        "\\\"command\\\":{\\\"type\\\":\\\"action\\\",\\\"name\\\":\\\"audio.adjust_volume\\\","
        "\\\"params\\\":{\\\"delta\\\":-10}}}\", \"data_type\": \"message\"}";
    assert(sizeof(captured) - 1U == 490);
    emit_device_command(NULL, "agent-uid", "cmd-adjust-base", "conversation-1", s_volume_command,
                        NULL);
    drain_device_commands();
    assert(read_counter(&s_last_media_volume) == 50);
    int sets_before = read_counter(&s_media_volume_set_calls);
    emit_rtm_subscribe_data("rtc-channel", "agent-uid", captured, sizeof(captured) - 1U);
    drain_device_commands();
    assert(read_counter(&s_media_volume_set_calls) == sets_before);

    const char *conversation = strstr(captured, "conv_dMWMDV-aC2tZ");
    assert(conversation != NULL);
    char matching[sizeof(captured)];
    int length =
        snprintf(matching, sizeof(matching), "%.*sconversation-1%s", (int)(conversation - captured),
                 captured, conversation + strlen("conv_dMWMDV-aC2tZ"));
    assert(length > 0 && (size_t)length < sizeof(matching));
    emit_rtm_subscribe_data("rtc-channel", "other-uid", matching, (size_t)length);
    emit_rtm_subscribe_data("bad-channel", "agent-uid", matching, (size_t)length);
    emit_rtm_data("other-uid", matching, (size_t)length);
    drain_device_commands();
    assert(read_counter(&s_media_volume_set_calls) == sets_before);
    emit_rtm_subscribe_data("rtc-channel", "agent-uid", matching, (size_t)length);
    drain_device_commands();
    assert(read_counter(&s_media_volume_set_calls) == sets_before + 1);
    assert(read_counter(&s_last_media_volume) == 40);

    /* A repeated relative command must not reduce the volume twice across routes. */
    emit_rtm_data("agent-uid", matching, (size_t)length);
    emit_rtm_subscribe_data("rtc-channel", "agent-uid", matching, (size_t)length);
    drain_device_commands();
    assert(read_counter(&s_media_volume_set_calls) == sets_before + 1);
    assert(read_counter(&s_last_media_volume) == 40);

    static const struct {
        int delta;
        int expected;
    } adjustments[] = {
        {-10, 30},  {10, 40},  {-100, 0}, {-10, 0}, {10, 10},
        {100, 100}, {10, 100}, {-10, 90}, {0, 90},
    };
    for (size_t i = 0; i < sizeof(adjustments) / sizeof(adjustments[0]); ++i) {
        char id[32];
        char command[96];
        int id_length = snprintf(id, sizeof(id), "cmd-adjust-sequence-%zu", i);
        int command_length = snprintf(
            command, sizeof(command),
            "{\"type\":\"action\",\"name\":\"audio.adjust_volume\",\"params\":{\"delta\":%d}}",
            adjustments[i].delta);
        assert(id_length > 0 && (size_t)id_length < sizeof(id));
        assert(command_length > 0 && (size_t)command_length < sizeof(command));
        emit_device_command(i % 2 ? "rtc-channel" : NULL, "agent-uid", id, "conversation-1",
                            command, NULL);
        drain_device_commands();
        assert(read_counter(&s_media_volume_set_calls) == sets_before + 2 + (int)i);
        assert(read_counter(&s_last_media_volume) == adjustments[i].expected);
    }
    /* Absolute volume remains supported and shares the same deduplication cache. */
    emit_device_command(NULL, "agent-uid", "cmd-adjust-restore", "conversation-1", s_volume_command,
                        NULL);
    drain_device_commands();
    int sets_after_restore = read_counter(&s_media_volume_set_calls);
    assert(sets_after_restore == sets_before + 11);
    assert(read_counter(&s_last_media_volume) == 50);
    emit_device_command(
        "rtc-channel", "agent-uid", "cmd-adjust-restore", "conversation-1",
        "{\"type\":\"action\",\"name\":\"audio.adjust_volume\",\"params\":{\"delta\":-10}}", NULL);
    drain_device_commands();
    assert(read_counter(&s_media_volume_set_calls) == sets_after_restore);
    assert(read_counter(&s_last_media_volume) == 50);
}

static void test_device_commands(void) {
    test_cloud_message_envelope();
    test_cloud_adjust_volume();
    int volume_calls_before = read_counter(&s_media_volume_set_calls);
    /* Unsupported commands and invalid levels must not consume the valid ID. */
    static const char *const unsupported[] = {
        "{\"type\":\"action\",\"name\":\"audio.adjust_volume\",\"params\":{\"delta\":-101}}",
        "{\"type\":\"action\",\"name\":\"audio.adjust_volume\",\"params\":{\"delta\":101}}",
        "{\"type\":\"action\",\"name\":\"audio.set_mute\",\"params\":{\"muted\":true}}",
        "{\"type\":\"scene\",\"name\":\"emotion.happy\",\"params\":{}}",
        "{\"type\":\"action\",\"name\":\"audio.set_volume\",\"params\":{\"level\":101}}",
        "{\"type\":\"action\",\"name\":\"audio.set_volume\",\"params\":{\"level\":-1}}",
    };
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
        emit_device_command("rtc-channel", "agent-uid", "cmd-volume", "conversation-1",
                            unsupported[i], NULL);
    }
    emit_device_command(NULL, "other-uid", "cmd-volume", "conversation-1", s_volume_command, NULL);
    emit_device_command("", "agent-uid", "cmd-volume", "conversation-1", s_volume_command, NULL);
    emit_device_command(NULL, "agent-uid", "cmd-volume", "conversation-2", s_volume_command, NULL);
    emit_device_command(NULL, "agent-uid", "cmd-volume\\u0000suffix", "conversation-1",
                        s_volume_command, NULL);
    drain_device_commands();
    assert(read_counter(&s_media_volume_set_calls) == volume_calls_before);

    /* Timestamp metadata does not gate volume execution. */
    const char *const metadata[] = {
        ",\"issued_at\":\"1970-01-01T00:00:00Z\",\"expires_at\":\"1970-01-01T00:00:01Z\"",
        ",\"issued_at\":\"9999-01-01T00:00:00Z\",\"expires_at\":\"9999-01-01T00:00:01Z\"",
        ",\"issued_at\":\"9999-01-01T00:00:00Z\",\"expires_at\":\"1970-01-01T00:00:00Z\"",
    };
    for (size_t i = 0; i < sizeof(metadata) / sizeof(metadata[0]); ++i) {
        char id[32];
        int length = snprintf(id, sizeof(id), "cmd-volume-time-%zu", i);
        assert(length > 0 && (size_t)length < sizeof(id));
        emit_device_command(NULL, "agent-uid", id, "conversation-1", s_volume_command, metadata[i]);
    }
    drain_device_commands();
    assert(read_counter(&s_media_volume_set_calls) == volume_calls_before + 3);

    emit_device_command(NULL, "agent-uid", "cmd-volume", "conversation-1", s_volume_command, NULL);
    drain_device_commands();
    assert(read_counter(&s_media_volume_set_calls) == volume_calls_before + 4);
    assert(read_counter(&s_last_media_volume) == 50);
    emit_device_command("rtc-channel", "agent-uid", "cmd-volume", "conversation-1",
                        s_volume_command, NULL);
    drain_device_commands();
    assert(read_counter(&s_media_volume_set_calls) == volume_calls_before + 4);

    /* The last 16 accepted IDs remain deduplicated while new IDs replace the oldest. */
    for (int index = 0; index < 21; ++index) {
        char id[32];
        assert(snprintf(id, sizeof(id), "cmd-fill-%d", index) > 0);
        emit_device_command(NULL, "agent-uid", id, "conversation-1", s_volume_command, NULL);
    }
    drain_device_commands();
    assert(read_counter(&s_media_volume_set_calls) == volume_calls_before + 25);
    emit_device_command(NULL, "agent-uid", "cmd-fill-20", "conversation-1", s_volume_command, NULL);
    emit_device_command(NULL, "agent-uid", "cmd-fill-5", "conversation-1", s_volume_command, NULL);
    drain_device_commands();
    assert(read_counter(&s_media_volume_set_calls) == volume_calls_before + 25);
    /* A duplicate does not refresh its FIFO position: the next new ID evicts cmd-fill-5. */
    emit_device_command(NULL, "agent-uid", "cmd-fill-21", "conversation-1", s_volume_command, NULL);
    emit_device_command(NULL, "agent-uid", "cmd-fill-5", "conversation-1", s_volume_command, NULL);
    emit_device_command(NULL, "agent-uid", "cmd-fill-0", "conversation-1", s_volume_command, NULL);
    drain_device_commands();
    assert(read_counter(&s_media_volume_set_calls) == volume_calls_before + 28);
    /* The next conversation will reuse this ID after its history has been cleared. */
    emit_device_command(
        NULL, "agent-uid", "cmd-volume-session-cache", "conversation-1",
        "{\"type\":\"action\",\"name\":\"audio.set_volume\",\"params\":{\"level\":100}}", NULL);
    drain_device_commands();
    assert(read_counter(&s_media_volume_set_calls) == volume_calls_before + 29);
    assert(read_counter(&s_last_media_volume) == 100);
}

static void test_lost_rtc_notification(const mybot_config_t *config) {
    int joins = read_counter(&s_rtc_join_calls);
    int ended = read_counter(&s_conversation_ended_notifications);
    assert(mybot_start(config) == 0);
    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
    emit_key_event(MYBOT_KEY_EVENT_CONVERSATION_START);
    assert(wait_for_counter(&s_rtc_join_calls, joins + 1, 1000));
    aosl_atomic_set(&s_rtc_notification_failed, true);
    assert(wait_for_counter(&s_conversation_ended_notifications, ended + 1, 1000));
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
    mybot_stop();
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);
}

static void test_invalid_config(const mybot_config_t *config) {
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);
    assert(mybot_start(NULL) < 0);
    mybot_config_t invalid = *config;
    invalid.server_base[0] = '\0';
    assert(mybot_start(&invalid) < 0);
    invalid = *config;
    invalid.device_id[0] = '\0';
    assert(mybot_start(&invalid) < 0);
    invalid = *config;
    memset(invalid.server_base, 'x', sizeof(invalid.server_base));
    assert(mybot_start(&invalid) < 0);
    invalid = *config;
    memset(invalid.device_id, 'x', sizeof(invalid.device_id));
    assert(mybot_start(&invalid) < 0);
    invalid = *config;
    memset(invalid.firmware_ver, 'x', sizeof(invalid.firmware_ver));
    assert(mybot_start(&invalid) < 0);
    invalid = *config;
    memset(invalid.hw_model, 'x', sizeof(invalid.hw_model));
    assert(mybot_start(&invalid) < 0);
    invalid = *config;
    snprintf(invalid.server_base, sizeof(invalid.server_base), "%s", "ftp://server");
    assert(mybot_start(&invalid) < 0);
    const char *invalid_servers[] = {
        "https://",
        "https:///path",
        "https://bad@host",
        "https://bad host",
        "https://host:",
        "https://host:-1",
        "https://host:0",
        "https://host:65536",
        "https://host/path#fragment",
        "https://host/path\\escape",
        "https://host/path\n",
    };
    for (size_t i = 0; i < sizeof(invalid_servers) / sizeof(invalid_servers[0]); ++i) {
        invalid = *config;
        assert(snprintf(invalid.server_base, sizeof(invalid.server_base), "%s",
                        invalid_servers[i]) < (int)sizeof(invalid.server_base));
        assert(mybot_start(&invalid) < 0);
        assert(mybot_get_state() == MYBOT_STATE_STOPPED);
    }
}

static void test_required_platform(const mybot_config_t *config) {
    s_platform_registered = false;
    assert(mybot_start(config) < 0);
    assert(!mybot_is_running());
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);
    assert(read_counter(&s_wifi_init_calls) == 0);

    s_platform_registered = true;
#if MYBOT_WAKE_WORDS
    s_wake_words_registered = false;
    assert(mybot_start(config) < 0);
    assert(!mybot_is_running());
    assert(read_counter(&s_wifi_init_calls) == 0);
    s_wake_words_registered = true;
#endif
#if MYBOT_ENABLE_HTTPS
    s_https_registered = false;
    assert(mybot_start(config) < 0);
    assert(!mybot_is_running());
    assert(read_counter(&s_wifi_init_calls) == 0);
    s_https_registered = true;
#endif
}

static void start_application_and_services(const mybot_config_t *config) {
    aosl_set_log_level(AOSL_LOG_ERROR);
    begin_control_thread_tracking();
    assert(mybot_start(config) == 0);
    assert(aosl_get_log_level() == AOSL_LOG_NOTICE);
    assert(mybot_is_running());
    assert(mybot_get_state() == MYBOT_STATE_WIFI_PROVISIONING);
    assert(read_counter(&s_wifi_init_calls) == 1);
    assert(mybot_start(config) < 0);

    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_READY, 3000));
    assert(read_counter(&s_kv_init_calls) == 1);
    assert(read_counter(&s_key_init_calls) == 1);
    assert(read_counter(&s_capture_init_calls) == 1);
    assert(read_counter(&s_capture_start_calls) == 1);
    assert(read_counter(&s_playback_init_calls) == 1);
    assert(read_counter(&s_playback_start_calls) == 1);
    assert(read_counter(&s_device_init_calls) == 1);
}

static void check_ready_keys_and_connectivity(void) {
    emit_key_event(MYBOT_KEY_EVENT_PAIR);
    emit_key_event(MYBOT_KEY_EVENT_VOLUME_UP);
    emit_key_event(MYBOT_KEY_EVENT_VOLUME_DOWN);
    assert(wait_for_counter(&s_pair_requests, 1, 1000));
    assert(wait_for_counter(&s_pair_callback_calls, 1, 1000));
    assert(wait_for_counter(&s_media_volume_set_calls, 2, 1000));
    assert(read_counter(&s_last_media_volume) == 90);

    emit_wifi_event(MYBOT_WIFI_EVENT_STA_DISCONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_WIFI_DISCONNECTED, 1000));
    assert(read_counter(&s_network_down_calls) == 1);
    assert(mybot_is_running());

    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
    assert(read_counter(&s_network_up_calls) == 1);
    assert(read_counter(&s_device_init_calls) == 1);

    emit_wifi_event(MYBOT_WIFI_EVENT_FAILED);
    assert(wait_for_app_state(MYBOT_STATE_WIFI_DISCONNECTED, 1000));
    assert(read_counter(&s_network_down_calls) == 2);
    int network_sets_before_duplicate = read_counter(&s_network_set_calls);
    emit_wifi_event(MYBOT_WIFI_EVENT_FAILED);
    assert(wait_for_counter(&s_network_set_calls, network_sets_before_duplicate + 1, 1000));
    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
    assert(read_counter(&s_network_down_calls) == 2);
    assert(read_counter(&s_network_up_calls) == 2);
}

static void start_first_conversation(void) {
    emit_key_event(MYBOT_KEY_EVENT_CONVERSATION_START);
    assert(wait_for_counter(&s_rtc_join_calls, 1, 3000));
#if MYBOT_ENABLE_VIDEO
    assert(wait_for_video_counter(&s_video_start_calls, 1));
    s_rtc_callbacks.on_video_key_frame_requested(s_rtc_callbacks.user_data);
    s_rtc_callbacks.on_video_target_bitrate_changed(96000U, s_rtc_callbacks.user_data);
    assert(wait_for_video_counter(&s_video_key_frame_calls, 1));
    assert(wait_for_video_counter(&s_video_bitrate_calls, 1));
#endif
    mock_lock();
    assert(strcmp(s_join_channel, "rtc-channel") == 0);
    assert(strcmp(s_join_user, "device-uid") == 0);
    mock_unlock();
    assert(s_rtc_callbacks.on_rtm_event != NULL);
    assert(s_rtc_callbacks.on_rtm_data != NULL);
    assert(s_rtc_callbacks.on_rtm_subscribe_result != NULL);
    assert(s_rtc_callbacks.on_rtm_subscribe_data != NULL);
}

static const char s_vp_registered_message[] =
    "{\"object\":\"message.sal_status\",\"status\":\"VP_REGISTER_SUCCESS\","
    "\"timestamp\":1710000000000,\"data_type\":\"message\","
    "\"message_id\":\"abcd1234\",\"send_ts\":1710000000100}";
static const char s_thinking_message[] =
    "{\"event_type\":\"state.thinking\",\"payload\":{\"value\":true}}";

static void check_invalid_indicator_messages(void) {
    const char malformed_vp_message[] =
        "{\"object\":\"message.sal_status\",\"status\":\"VP_REGISTER_FAILED\"}";
    emit_rtm_subscribe_data("rtc-channel", "agent-uid", malformed_vp_message,
                            sizeof(malformed_vp_message) - 1U);
    wait_for_control_events();
    assert(wait_for_lcd_indicator_state(MYBOT_LCD_INDICATOR_VP_REGISTERED, false, 1000));
    emit_rtm_subscribe_data("rtc-channel", "other-uid", s_vp_registered_message,
                            sizeof(s_vp_registered_message) - 1U);
    wait_for_control_events();
    assert(wait_for_lcd_indicator_state(MYBOT_LCD_INDICATOR_VP_REGISTERED, false, 1000));

    const char uppercase_key_message[] =
        "{\"Object\":\"message.sal_status\",\"status\":\"VP_REGISTER_SUCCESS\"}";
    emit_rtm_subscribe_data("rtc-channel", "agent-uid", uppercase_key_message,
                            sizeof(uppercase_key_message) - 1U);
    wait_for_control_events();
    assert(wait_for_lcd_indicator_state(MYBOT_LCD_INDICATOR_VP_REGISTERED, false, 1000));

    const char *invalid_messages[] = {
        "{",
        "[]",
        "null",
        "{\"event_type\":\"state.unknown\",\"payload\":{\"value\":true}}",
        "{\"event_type\":\"state.listening\",\"payload\":true}",
        "{\"event_type\":\"state.listening\",\"payload\":{\"value\":1}}",
        "{\"event_type\":\"state.listening\",\"payload\":{}}",
        "{\"event_type\":true,\"payload\":{\"value\":true}}",
    };
    for (size_t i = 0; i < sizeof(invalid_messages) / sizeof(invalid_messages[0]); ++i) {
        emit_rtm_subscribe_data("rtc-channel", "agent-uid", invalid_messages[i],
                                strlen(invalid_messages[i]));
    }
    char long_channel[129];
    memset(long_channel, 'c', sizeof(long_channel) - 1);
    long_channel[sizeof(long_channel) - 1] = '\0';
    char long_sender[MYBOT_RTM_UID_MAX_LEN + 1];
    memset(long_sender, 'u', sizeof(long_sender) - 1);
    long_sender[sizeof(long_sender) - 1] = '\0';
    emit_rtm_subscribe_data(NULL, "agent-uid", s_vp_registered_message,
                            sizeof(s_vp_registered_message) - 1U);
    emit_rtm_subscribe_data(long_channel, "agent-uid", s_vp_registered_message,
                            sizeof(s_vp_registered_message) - 1U);
    emit_rtm_subscribe_data("rtc-channel", NULL, s_vp_registered_message,
                            sizeof(s_vp_registered_message) - 1U);
    emit_rtm_subscribe_data("rtc-channel", long_sender, s_vp_registered_message,
                            sizeof(s_vp_registered_message) - 1U);
    emit_rtm_subscribe_data("rtc-channel", "agent-uid", NULL, 1);
    emit_rtm_subscribe_data("rtc-channel", "agent-uid", s_vp_registered_message, 0);
    emit_rtm_subscribe_data("rtc-channel", "agent-uid", s_vp_registered_message,
                            sizeof(s_vp_registered_message));
    static const char oversized_message[1025] = {0};
    emit_rtm_subscribe_data("rtc-channel", "agent-uid", oversized_message,
                            sizeof(oversized_message));
    emit_rtm_data(NULL, s_vp_registered_message, sizeof(s_vp_registered_message) - 1U);
    emit_rtm_data(long_sender, s_vp_registered_message, sizeof(s_vp_registered_message) - 1U);
    wait_for_control_events();
    mock_lock();
    assert(s_last_lcd_content.screen == MYBOT_LCD_SCREEN_IN_CONVERSATION);
    assert(s_last_lcd_content.indicators == MYBOT_LCD_INDICATOR_NONE);
    mock_unlock();

    emit_rtm_data("agent-uid", s_vp_registered_message, sizeof(s_vp_registered_message) - 1U);
    wait_for_control_events();
    assert(wait_for_lcd_indicator_state(MYBOT_LCD_INDICATOR_VP_REGISTERED, false, 1000));

    emit_rtm_subscribe_data("bad-channel", "agent-uid", s_vp_registered_message,
                            sizeof(s_vp_registered_message) - 1U);
    wait_for_control_events();
    assert(wait_for_lcd_indicator_state(MYBOT_LCD_INDICATOR_VP_REGISTERED, false, 1000));
}

static void check_conversation_indicators_and_commands(void) {
    const char listening_message[] =
        "{\"event_type\":\"state.listening\",\"payload\":{\"value\":true}}";
    emit_rtm_subscribe_data("rtc-channel", "agent-uid", listening_message,
                            sizeof(listening_message) - 1U);
    assert(wait_for_lcd_indicator(MYBOT_LCD_INDICATOR_LISTENING, 1000));
    assert(wait_for_lcd_indicator_state(MYBOT_LCD_INDICATOR_THINKING, false, 1000));

    /* Voiceprint registration is independent of the current server state. */
    emit_rtm_subscribe_data("rtc-channel", "agent-uid", s_vp_registered_message,
                            sizeof(s_vp_registered_message) - 1U);
    assert(wait_for_lcd_indicator(MYBOT_LCD_INDICATOR_VP_REGISTERED, 1000));
    mock_lock();
    assert(s_last_lcd_content.indicators ==
           (MYBOT_LCD_INDICATOR_VP_REGISTERED | MYBOT_LCD_INDICATOR_LISTENING));
    mock_unlock();
    emit_rtm_subscribe_data("rtc-channel", "agent-uid", s_thinking_message,
                            sizeof(s_thinking_message) - 1U);
    assert(wait_for_lcd_indicator(MYBOT_LCD_INDICATOR_THINKING, 1000));
    assert(wait_for_lcd_indicator_state(MYBOT_LCD_INDICATOR_LISTENING, false, 1000));

    /* A delayed false for the preceding state must not clear its successor. */
    const char listening_done_message[] =
        "{\"event_type\":\"state.listening\",\"payload\":{\"value\":false}}";
    emit_rtm_subscribe_data("rtc-channel", "agent-uid", listening_done_message,
                            sizeof(listening_done_message) - 1U);
    wait_for_control_events();
    mock_lock();
    assert(s_last_lcd_content.indicators ==
           (MYBOT_LCD_INDICATOR_VP_REGISTERED | MYBOT_LCD_INDICATOR_THINKING));
    mock_unlock();

    const char speaking_message[] =
        "{\"event_type\":\"state.speaking\",\"payload\":{\"value\":true}}";
    emit_rtm_subscribe_data("rtc-channel", "agent-uid", speaking_message,
                            sizeof(speaking_message) - 1U);
    assert(wait_for_lcd_indicator(MYBOT_LCD_INDICATOR_SPEAKING, 1000));

    /* Keep a different-length sender case alongside the same-length VP case. */
    emit_rtm_subscribe_data("rtc-channel", "other-agent", s_thinking_message,
                            sizeof(s_thinking_message) - 1U);
    wait_for_control_events();
    assert(wait_for_lcd_indicator(MYBOT_LCD_INDICATOR_SPEAKING, 1000));
    assert(wait_for_lcd_indicator_state(MYBOT_LCD_INDICATOR_THINKING, false, 1000));

    test_device_commands();

    const char speaking_done_message[] =
        "{\"event_type\":\"state.speaking\",\"payload\":{\"value\":false}}";
    emit_rtm_subscribe_data("rtc-channel", "agent-uid", speaking_done_message,
                            sizeof(speaking_done_message) - 1U);
    assert(wait_for_lcd_indicator_state(MYBOT_LCD_INDICATOR_SPEAKING, false, 1000));
}

static void check_audio_uplink_and_playback(void) {
    assert(wait_for_counter(&s_rtc_send_calls, 1, 3000));
    assert(wait_for_audio_frame(0, 1000));

#if MYBOT_CLOUD_AEC
    int16_t reference_frame[TEST_FRAME_SAMPLES];
    for (int i = 0; i < TEST_FRAME_SAMPLES; i++) {
        reference_frame[i] = TEST_REF_SAMPLE;
    }

    mock_lock();
    s_announce_active = true;
    mock_unlock();
    int playback_writes_before_announcement = read_counter(&s_playback_write_calls);
    emit_remote_audio(reference_frame, sizeof(reference_frame));
    aosl_hal_msleep(100);
    assert(read_counter(&s_playback_write_calls) == playback_writes_before_announcement);

    mock_lock();
    s_announce_active = false;
    mock_unlock();
    emit_remote_audio(reference_frame, sizeof(reference_frame));
    assert(wait_for_counter(&s_playback_write_calls, 1, 1000));
    assert(wait_for_audio_frame(TEST_REF_SAMPLE, 2000));

    /* A short playback write must enqueue only the samples accepted by the
     * device; the second half is committed on the following playback tick. */
    mock_lock();
    s_playback_write_limit = TEST_FRAME_SAMPLES / 2;
    mock_unlock();
    int short_write_calls_before = read_counter(&s_playback_write_calls);
    int short_send_calls_before = read_counter(&s_rtc_send_calls);
    emit_remote_audio(reference_frame, sizeof(reference_frame));
    assert(wait_for_counter(&s_playback_write_calls, short_write_calls_before + 2, 1000));
    assert(wait_for_audio_frame_from(TEST_REF_SAMPLE, short_send_calls_before, 2000));
    mock_lock();
    s_playback_write_limit = -1;
    mock_unlock();
#endif
}

static void hang_up_and_check_late_notifications(void) {
    emit_key_event(MYBOT_KEY_EVENT_CONVERSATION_STOP);
    assert(wait_for_counter(&s_stop_requests, 1, 1000));
    assert(wait_for_counter(&s_rtc_leave_calls, 1, 1000));
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
    assert(wait_for_lcd_indicator_state(MYBOT_LCD_INDICATOR_VP_REGISTERED, false, 1000));
    int volume_sets_after_stop = read_counter(&s_media_volume_set_calls);
    emit_device_command(NULL, "agent-uid", "cmd-after-stop", "conversation-1", s_volume_command,
                        NULL);
    emit_device_command(
        "rtc-channel", "agent-uid", "cmd-volume-after-stop", "conversation-1",
        "{\"type\":\"action\",\"name\":\"audio.set_volume\",\"params\":{\"level\":0}}", NULL);
    drain_device_commands();
    assert(read_counter(&s_media_volume_set_calls) == volume_sets_after_stop);
    /* Late notifications cannot restore the ended conversation's screen. */
    emit_rtm_subscribe_data("rtc-channel", "agent-uid", s_vp_registered_message,
                            sizeof(s_vp_registered_message) - 1U);
    emit_rtm_subscribe_data("rtc-channel", "agent-uid", s_thinking_message,
                            sizeof(s_thinking_message) - 1U);
    wait_for_control_events();
    mock_lock();
    assert(s_last_lcd_content.screen == MYBOT_LCD_SCREEN_READY);
    assert(s_last_lcd_content.indicators == MYBOT_LCD_INDICATOR_NONE);
    mock_unlock();
#if MYBOT_ENABLE_VIDEO
    /* Notifications arriving after hangup cannot restart or reconfigure video.
     * A following volume event confirms that control consumed the notifications. */
    emit_rtc_connected();
    s_rtc_callbacks.on_video_key_frame_requested(s_rtc_callbacks.user_data);
    s_rtc_callbacks.on_video_target_bitrate_changed(96000U, s_rtc_callbacks.user_data);
    wait_for_control_events();
    assert(aosl_atomic_read(&s_video_start_calls) == 1);
    assert(aosl_atomic_read(&s_video_key_frame_calls) == 1);
    assert(aosl_atomic_read(&s_video_bitrate_calls) == 1);
    s_rtc_callbacks.on_state_changed(MYBOT_RTC_STATE_INITIALIZED, s_rtc_callbacks.user_data);
#endif
}

static void start_second_conversation(void) {
    int sends_before_second_call = read_counter(&s_rtc_send_calls);
#if MYBOT_WAKE_WORDS
    emit_wake_word();
#else
    emit_key_event(MYBOT_KEY_EVENT_CONVERSATION_START);
#endif
    assert(wait_for_counter(&s_rtc_join_calls, 2, 3000));
    assert(wait_for_counter(&s_rtc_send_calls, sends_before_second_call + 1, 3000));
    assert(read_counter(&s_rtc_init_calls) == 2);
    assert(read_counter(&s_rtc_leave_calls) == 1);
    int volume_sets_before_second_command = read_counter(&s_media_volume_set_calls);
    emit_device_command(NULL, "agent-uid", "cmd-volume-session-cache", "conversation-1",
                        s_volume_command, NULL);
    drain_device_commands();
    assert(read_counter(&s_media_volume_set_calls) == volume_sets_before_second_command + 1);
    assert(read_counter(&s_last_media_volume) == 50);
}

static void block_capture_and_playback(void) {
    /* Wait until playback applies the pending buffer clear before blocking I/O. */
    int activity_checks = read_counter(&s_announce_active_checks);
    assert(wait_for_counter(&s_announce_active_checks, activity_checks + 1, 1000));

    int16_t shutdown_frame[TEST_FRAME_SAMPLES];
    memset(shutdown_frame, 0, sizeof(shutdown_frame));
    mock_lock();
    s_block_capture_read = true;
    s_block_playback_write = true;
    mock_unlock();
    emit_remote_audio(shutdown_frame, sizeof(shutdown_frame));
    assert(wait_for_flag(&s_capture_read_blocked, 1000));
    assert(wait_for_flag(&s_playback_write_blocked, 2000));
}

static void stop_and_check_resource_cleanup(void) {
    mybot_stop();
    end_control_thread_tracking();
    assert(!s_capture_read_timed_out);
    assert(!s_playback_write_timed_out);
    assert(!mybot_is_running());
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);
    assert(read_counter(&s_device_shutdown_calls) == 1);
    assert(read_counter(&s_rtc_init_calls) == 2);
    assert(read_counter(&s_rtc_join_calls) == 2);
    assert(read_counter(&s_rtc_leave_calls) == 2);
    assert(read_counter(&s_rtc_fini_calls) == 1);
    assert(read_counter(&s_capture_stop_calls) == 1);
#if MYBOT_ENABLE_VIDEO
    assert(aosl_atomic_read(&s_video_init_calls) == 1);
    assert(aosl_atomic_read(&s_video_start_calls) == 2);
    assert(aosl_atomic_read(&s_video_stop_calls) == 2);
    assert(aosl_atomic_read(&s_video_destroy_calls) == 1);
#endif
    assert(read_counter(&s_capture_destroy_calls) == 1);
    assert(read_counter(&s_playback_stop_calls) == 1);
    assert(read_counter(&s_playback_destroy_calls) == 1);
    assert(read_counter(&s_kv_deinit_calls) == 1);
    assert(read_counter(&s_key_deinit_calls) == 1);
    assert(read_counter(&s_wifi_deinit_calls) == 1);

    mybot_device_conversation_t late_params;
    memset(&late_params, 0, sizeof(late_params));
    int notifications_before_late = read_counter(&s_conversation_ended_notifications);
    s_device_callbacks.on_conversation_start(&late_params, s_device_callbacks.user_data);
    assert(read_counter(&s_conversation_ended_notifications) == notifications_before_late + 1);

    mybot_stop();
    assert(read_counter(&s_device_shutdown_calls) == 1);
    assert(read_counter(&s_rtc_fini_calls) == 1);
}

static void test_two_conversations(const mybot_config_t *config) {
    start_application_and_services(config);
    check_ready_keys_and_connectivity();
    start_first_conversation();
    check_invalid_indicator_messages();
    check_conversation_indicators_and_commands();

    emit_key_event(MYBOT_KEY_EVENT_PAIR);
    assert(wait_for_counter(&s_pair_requests, 2, 1000));

    mock_lock();
    void (*token_will_expire)(void *) = s_rtc_callbacks.on_token_will_expire;
    void *rtc_user_data = s_rtc_callbacks.user_data;
    int (*token_renewed)(const char *, void *) = s_device_callbacks.on_rtc_token_renewed;
    void *device_user_data = s_device_callbacks.user_data;
    mock_unlock();
    assert(token_will_expire != NULL);
    assert(token_renewed != NULL);

    token_will_expire(rtc_user_data);
    assert(wait_for_counter(&s_token_renewal_requests, 1, 1000));
    assert(wait_for_counter(&s_rtc_renew_calls, 1, 1000));
    mock_lock();
    assert(strcmp(s_renewed_token, "renewed-token") == 0);
    mock_unlock();

    check_audio_uplink_and_playback();
    hang_up_and_check_late_notifications();
    start_second_conversation();
    block_capture_and_playback();

    emit_key_event(MYBOT_KEY_EVENT_EXIT);
    assert(!mybot_is_running());
    int renewals_before_stop = read_counter(&s_token_renewal_requests);
    token_will_expire(rtc_user_data);
    assert(read_counter(&s_token_renewal_requests) == renewals_before_stop);
    assert(token_renewed("too-late", device_user_data) < 0);
    stop_and_check_resource_cleanup();
}

static void test_startup_failure_states(const mybot_config_t *config) {
    aosl_set_log_level(AOSL_LOG_DEBUG);
    s_wifi_init_fails = true;
    assert(mybot_start(config) < 0);
    assert(aosl_get_log_level() == AOSL_LOG_DEBUG);
    s_wifi_init_fails = false;
    assert(!mybot_is_running());
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);

    s_kv_init_fails = true;
    assert(mybot_start(config) == 0);
    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_FAILED, 1000));
    assert(wait_for_runtime_exit(1000));
    mybot_stop();
    s_kv_init_fails = false;
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);
}

static void test_stop_during_start(mybot_config_t *config) {
    /* Concurrent stop must wait for a start that is still constructing the
     * runtime instead of tearing down partially initialized resources. */
    mock_lock();
    s_block_wifi_init = true;
    s_wifi_init_entered = false;
    s_start_thread_returned = false;
    s_stop_thread_entered = false;
    s_stop_thread_returned = false;
    s_start_thread_result = -1;
    int wifi_deinit_before_race = s_wifi_deinit_calls;
    mock_unlock();

    pthread_t start_tid;
    pthread_t stop_tid;
    int thread_result = pthread_create(&start_tid, NULL, start_thread, config);
    assert(thread_result == 0);
    assert(wait_for_flag_value(&s_wifi_init_entered, true, 1000));
    thread_result = pthread_create(&stop_tid, NULL, stop_thread, NULL);
    assert(thread_result == 0);
    assert(wait_for_flag_value(&s_stop_thread_entered, true, 1000));
    aosl_hal_msleep(20);
    assert(!read_bool(&s_stop_thread_returned));
    assert(read_counter(&s_wifi_deinit_calls) == wifi_deinit_before_race);

    mock_lock();
    s_block_wifi_init = false;
    mock_unlock();
    assert(pthread_join(start_tid, NULL) == 0);
    assert(pthread_join(stop_tid, NULL) == 0);
    assert(read_bool(&s_start_thread_returned));
    assert(read_counter(&s_start_thread_result) == 0);
    assert(read_bool(&s_stop_thread_returned));
    assert(read_counter(&s_wifi_deinit_calls) == wifi_deinit_before_race + 1);
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);
}

static void test_rtc_start_failures(const mybot_config_t *config) {
    assert(mybot_start(config) == 0);
    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
    int ended_before_failure = read_counter(&s_conversation_ended_notifications);
    mock_lock();
    s_rtc_join_result = -1;
    mock_unlock();
    int joins_before_failure = read_counter(&s_rtc_join_calls);
    emit_key_event(MYBOT_KEY_EVENT_CONVERSATION_START);
    assert(wait_for_counter(&s_rtc_join_calls, joins_before_failure + 1, 1000));
    assert(wait_for_counter(&s_conversation_ended_notifications, ended_before_failure + 1, 1000));
    mock_lock();
    s_rtc_join_result = 0;
    mock_unlock();
    mybot_stop();

    assert(mybot_start(config) == 0);
    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
    ended_before_failure = read_counter(&s_conversation_ended_notifications);
    mock_lock();
    s_rtc_init_result = -1;
    mock_unlock();
    int inits_before_failure = read_counter(&s_rtc_init_calls);
    emit_key_event(MYBOT_KEY_EVENT_CONVERSATION_START);
    assert(wait_for_counter(&s_rtc_init_calls, inits_before_failure + 1, 1000));
    assert(wait_for_counter(&s_conversation_ended_notifications, ended_before_failure + 1, 1000));
    mybot_stop();
    mock_lock();
    s_rtc_init_result = 0;
    mock_unlock();
}

static void test_wifi_connected_during_init(const mybot_config_t *config) {
    mock_lock();
    s_wifi_emit_connected_on_init = true;
    mock_unlock();
    assert(mybot_start(config) == 0);
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
    mybot_stop();
    mock_lock();
    s_wifi_emit_connected_on_init = false;
    mock_unlock();
}

#if MYBOT_ENABLE_VIDEO
static void test_video_init_failure(const mybot_config_t *config) {
    /* An encoder init failure must fail service startup and leave restart possible. */
    intptr_t video_destroys_before = aosl_atomic_read(&s_video_destroy_calls);
    aosl_atomic_set(&s_video_init_fails, true);
    assert(mybot_start(config) == 0);
    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_FAILED, 1000));
    mybot_stop();
    assert(aosl_atomic_read(&s_video_destroy_calls) == video_destroys_before);
    aosl_atomic_set(&s_video_init_fails, false);
    assert(mybot_start(config) == 0);
    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
    mybot_stop();
    assert(aosl_atomic_read(&s_video_destroy_calls) == video_destroys_before + 1);
}

static void test_video_stop_retry(const mybot_config_t *config) {
    /* Keep the original encoder owner for a retry after a failed stop. */
    assert(mybot_start(config) == 0);
    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
    intptr_t starts_before_retry = aosl_atomic_read(&s_video_start_calls);
    intptr_t video_destroys_before = aosl_atomic_read(&s_video_destroy_calls);
    emit_key_event(MYBOT_KEY_EVENT_CONVERSATION_START);
    assert(wait_for_video_counter(&s_video_start_calls, starts_before_retry + 1));
    aosl_atomic_set(&s_video_stop_fails, true);
    mybot_stop();
    assert(mybot_get_state() == MYBOT_STATE_STOPPING);
    assert(mybot_start(config) < 0);
    assert(aosl_atomic_read(&s_video_destroy_calls) == video_destroys_before);
    aosl_atomic_set(&s_video_stop_fails, false);
    mybot_stop();
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);
    assert(aosl_atomic_read(&s_video_destroy_calls) == video_destroys_before + 1);
}

static void test_stop_during_video_start(const mybot_config_t *config) {
    /* Shutdown must wait for a platform start already executing on control,
     * rather than invoking stop/destroy concurrently from another thread. */
    assert(mybot_start(config) == 0);
    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
    intptr_t video_starts_before = aosl_atomic_read(&s_video_start_calls);
    intptr_t video_stops_before = aosl_atomic_read(&s_video_stop_calls);
    intptr_t video_destroys_before = aosl_atomic_read(&s_video_destroy_calls);
    mock_lock();
    s_block_video_start = true;
    s_video_start_entered = false;
    s_stop_thread_entered = false;
    s_stop_thread_returned = false;
    mock_unlock();
    emit_key_event(MYBOT_KEY_EVENT_CONVERSATION_START);
    assert(wait_for_flag(&s_video_start_entered, 1000));
    assert(mybot_is_running());
    pthread_t stop_tid;
    int thread_result = pthread_create(&stop_tid, NULL, stop_thread, NULL);
    assert(thread_result == 0);
    assert(wait_for_flag(&s_stop_thread_entered, 1000));
    for (int elapsed = 0; elapsed < 1000 && mybot_is_running(); elapsed++) {
        aosl_hal_msleep(1);
    }
    assert(!mybot_is_running());
    assert(!read_bool(&s_stop_thread_returned));
    assert(aosl_atomic_read(&s_video_stop_calls) == video_stops_before);
    assert(aosl_atomic_read(&s_video_destroy_calls) == video_destroys_before);
    mock_lock();
    s_block_video_start = false;
    assert(pthread_cond_broadcast(&s_io_cond) == 0);
    mock_unlock();
    assert(pthread_join(stop_tid, NULL) == 0);
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);
    assert(aosl_atomic_read(&s_video_start_calls) == video_starts_before + 1);
    assert(aosl_atomic_read(&s_video_stop_calls) == video_stops_before + 1);
    assert(aosl_atomic_read(&s_video_destroy_calls) == video_destroys_before + 1);
}

#endif

int main(void) {
    mybot_config_t config = {.server_base = TEST_SERVER_BASE,
                             .device_id = "device-1",
                             .firmware_ver = "test-fw",
                             .hw_model = "test-hw"};
    s_https_registered = true;
    s_wake_words_registered = true;

    test_invalid_config(&config);
    test_required_platform(&config);
    test_two_conversations(&config);
    test_startup_failure_states(&config);
    test_stop_during_start(&config);
    test_rtc_start_failures(&config);
    test_wifi_connected_during_init(&config);
#if MYBOT_ENABLE_VIDEO
    test_video_init_failure(&config);
    test_video_stop_retry(&config);
    test_stop_during_video_start(&config);
#endif
    test_control_queue_saturation(&config);
    test_lost_rtc_notification(&config);
    test_service_startup_failures(&config);
    test_background_service_requests(&config);
#if MYBOT_TEST_WRAP_APP_MPQ
    test_media_join_retries(&config);
    test_control_queue_failures(&config);
#endif

    puts("app_test: ok");
    return 0;
}
