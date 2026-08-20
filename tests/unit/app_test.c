/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/mybot.h>
#include <mybot/mybot_build_config.h>
#include <mybot/platform/mybot_audio.h>
#include <mybot/platform/mybot_key.h>
#include <mybot/platform/mybot_lcd.h>
#include <mybot/platform/mybot_wake_words.h>
#include <mybot/platform/mybot_wifi.h>

#include "mybot_announce_internal.h"
#include "mybot_app.h"
#include "mybot_audio_internal.h"
#include "mybot_device_lifecycle.h"
#include "mybot_https_internal.h"
#include "mybot_key_internal.h"
#include "mybot_kv_store_internal.h"
#include "mybot_lcd_internal.h"
#include "mybot_rtc_session.h"
#include "mybot_wake_words_internal.h"
#include "mybot_wifi_internal.h"

#include <api/aosl.h>
#include <hal/aosl_hal_time.h>

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
#define TEST_MAX_SENT_FRAMES 32
#define TEST_MIC_BASE 1000
#define TEST_REF_SAMPLE (-2345)

#if MYBOT_ENABLE_HTTPS
#define TEST_SERVER_BASE "https://server"
#else
#define TEST_SERVER_BASE "http://server"
#endif

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static mybot_wifi_event_handler_t s_wifi_handler;
static void *s_wifi_user_data;

static mybot_device_lifecycle_callbacks_t s_device_callbacks;
static mybot_device_state_t s_device_state = MYBOT_DEVICE_STATE_RUNTIME;
static bool s_device_start_requested;
static bool s_device_network_available = true;

static mybot_rtc_session_callbacks_t s_rtc_callbacks;
static bool s_rtc_initialized;
static bool s_rtc_joined;

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
static int s_device_init_calls;
static int s_device_shutdown_calls;
static int s_network_down_calls;
static int s_network_up_calls;
static int s_rtc_init_calls;
static int s_rtc_join_calls;
static int s_rtc_leave_calls;
static int s_rtc_fini_calls;
static int s_rtc_send_calls;
static int s_rtc_renew_calls;
static int s_token_renewal_requests;
static char s_renewed_token[512];

static size_t s_sent_lengths[TEST_MAX_SENT_FRAMES];
static int16_t s_sent_frames[TEST_MAX_SENT_FRAMES][TEST_SEND_FRAME_BYTES / sizeof(int16_t)];
static char s_join_channel[128];
static char s_join_user[64];

static int s_capture_context;
static int s_playback_context;
static bool s_capture_started;
static bool s_playback_started;

static void mock_lock(void) {
    assert(pthread_mutex_lock(&s_lock) == 0);
}

static void mock_unlock(void) {
    assert(pthread_mutex_unlock(&s_lock) == 0);
}

static int read_counter(const int *counter) {
    mock_lock();
    int value = *counter;
    mock_unlock();
    return value;
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

static bool wait_for_counter(const int *counter, int minimum, int timeout_ms) {
    for (int elapsed = 0; elapsed < timeout_ms; elapsed++) {
        if (read_counter(counter) >= minimum) {
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

static bool wait_for_audio_frame(int16_t reference_sample, int timeout_ms) {
    for (int elapsed = 0; elapsed < timeout_ms; elapsed++) {
        int send_calls = read_counter(&s_rtc_send_calls);
        int limit = send_calls < TEST_MAX_SENT_FRAMES ? send_calls : TEST_MAX_SENT_FRAMES;
        for (int i = 0; i < limit; i++) {
            if (sent_frame_matches(i, reference_sample)) {
                return true;
            }
        }
        aosl_hal_msleep(1);
    }
    return false;
}

static void emit_wifi_event(mybot_wifi_event_t event) {
    mock_lock();
    mybot_wifi_event_handler_t handler = s_wifi_handler;
    void *user_data = s_wifi_user_data;
    mock_unlock();
    assert(handler != NULL);
    handler(event, user_data);
}

static void emit_remote_audio(const int16_t *pcm, size_t len) {
    mock_lock();
    void (*callback)(uint32_t, const void *, size_t) = s_rtc_callbacks.on_remote_audio;
    mock_unlock();
    assert(callback != NULL);
    callback(7, pcm, len);
}

bool mybot_https_is_registered(void) {
    return true;
}

bool mybot_lcd_is_registered(void) {
    return false;
}

int mybot_lcd_init(void) {
    return 0;
}

int mybot_lcd_show_screen(mybot_lcd_screen_t screen) {
    (void)screen;
    return 0;
}

int mybot_lcd_show_pair_code(const char *pair_code) {
    (void)pair_code;
    return 0;
}

void mybot_lcd_deinit(void) {
}

int mybot_kv_store_init(void) {
    mock_lock();
    s_kv_init_calls++;
    mock_unlock();
    return 0;
}

void mybot_kv_store_deinit(void) {
    mock_lock();
    s_kv_deinit_calls++;
    mock_unlock();
}

int mybot_key_init(mybot_key_event_handler_t handler, void *user_data) {
    (void)handler;
    (void)user_data;
    mock_lock();
    s_key_init_calls++;
    mock_unlock();
    return 0;
}

void mybot_key_deinit(void) {
    mock_lock();
    s_key_deinit_calls++;
    mock_unlock();
}

int mybot_announce_init(void) {
    return 0;
}

void mybot_announce_deinit(void) {
}

int mybot_announce_play_pair_code(const char *code) {
    (void)code;
    return 0;
}

void mybot_announce_stop(void) {
}

bool mybot_announce_is_active(void) {
    return false;
}

int mybot_announce_read_pcm(int16_t *dst, int max_frames) {
    (void)dst;
    (void)max_frames;
    return 0;
}

static int capture_init(void **ctx, int rate, int channels, int bits) {
    assert(rate == TEST_SAMPLE_RATE);
    assert(channels == TEST_CHANNELS);
    assert(bits == TEST_BITS_PER_SAMPLE);
    *ctx = &s_capture_context;
    mock_lock();
    s_capture_init_calls++;
    mock_unlock();
    return 0;
}

static int capture_start(void *ctx) {
    assert(ctx == &s_capture_context);
    mock_lock();
    s_capture_started = true;
    s_capture_start_calls++;
    mock_unlock();
    return 0;
}

static int capture_read(void *ctx, void *buf, int frames) {
    assert(ctx == &s_capture_context);
    mock_lock();
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
    mock_lock();
    s_capture_started = false;
    s_capture_stop_calls++;
    mock_unlock();
    return 0;
}

static void capture_destroy(void *ctx) {
    assert(ctx == &s_capture_context);
    mock_lock();
    s_capture_destroy_calls++;
    mock_unlock();
}

static int playback_init(void **ctx, int rate, int channels, int bits) {
    assert(rate == TEST_SAMPLE_RATE);
    assert(channels == TEST_CHANNELS);
    assert(bits == TEST_BITS_PER_SAMPLE);
    *ctx = &s_playback_context;
    mock_lock();
    s_playback_init_calls++;
    mock_unlock();
    return 0;
}

static int playback_start(void *ctx) {
    assert(ctx == &s_playback_context);
    mock_lock();
    s_playback_started = true;
    s_playback_start_calls++;
    mock_unlock();
    return 0;
}

static int playback_write(void *ctx, const void *buf, int frames) {
    assert(ctx == &s_playback_context);
    assert(buf != NULL);
    mock_lock();
    bool started = s_playback_started;
    if (started) {
        s_playback_write_calls++;
    }
    mock_unlock();
    return started ? frames : 0;
}

static int playback_stop(void *ctx) {
    assert(ctx == &s_playback_context);
    mock_lock();
    s_playback_started = false;
    s_playback_stop_calls++;
    mock_unlock();
    return 0;
}

static void playback_destroy(void *ctx) {
    assert(ctx == &s_playback_context);
    mock_lock();
    s_playback_destroy_calls++;
    mock_unlock();
}

static const mybot_audio_capture_ops_t s_capture_ops = {
    .name = "test-capture",
    .init = capture_init,
    .start = capture_start,
    .read = capture_read,
    .stop = capture_stop,
    .destroy = capture_destroy,
};

static const mybot_audio_playback_ops_t s_playback_ops = {
    .name = "test-playback",
    .init = playback_init,
    .start = playback_start,
    .write = playback_write,
    .stop = playback_stop,
    .destroy = playback_destroy,
};

const mybot_audio_capture_ops_t *mybot_audio_get_capture(void) {
    return &s_capture_ops;
}

const mybot_audio_playback_ops_t *mybot_audio_get_playback(void) {
    return &s_playback_ops;
}

int mybot_audio_device_volume_init(void) {
    return -1;
}

void mybot_audio_device_volume_deinit(void) {
}

bool mybot_audio_device_volume_is_active(void) {
    return false;
}

int mybot_audio_device_set_volume(int volume) {
    (void)volume;
    return -1;
}

int mybot_audio_device_get_volume(int *volume) {
    (void)volume;
    return -1;
}

int mybot_audio_set_media_volume(int volume) {
    return volume >= MYBOT_AUDIO_VOLUME_MIN && volume <= MYBOT_AUDIO_VOLUME_MAX ? 0 : -1;
}

int mybot_audio_get_media_volume(void) {
    return MYBOT_AUDIO_VOLUME_DEFAULT;
}

void mybot_audio_apply_media_volume(int16_t *pcm, int samples) {
    (void)pcm;
    (void)samples;
}

bool mybot_wake_words_is_registered(void) {
    return true;
}

int mybot_wake_words_init(int sample_rate, int channels, int bits_per_sample,
                          mybot_wake_words_handler_t handler, void *user_data) {
    (void)sample_rate;
    (void)channels;
    (void)bits_per_sample;
    (void)handler;
    (void)user_data;
    return 0;
}

int mybot_wake_words_process(const void *pcm, int frames) {
    (void)pcm;
    (void)frames;
    return 0;
}

void mybot_wake_words_deinit(void) {
}

int mybot_wifi_init(const char *device_id, mybot_wifi_event_handler_t handler, void *user_data) {
    if (!device_id || !handler) {
        return -1;
    }
    assert(strcmp(device_id, "device-1") == 0);
    mock_lock();
    s_wifi_handler = handler;
    s_wifi_user_data = user_data;
    s_wifi_init_calls++;
    mock_unlock();
    return 0;
}

void mybot_wifi_deinit(void) {
    mock_lock();
    s_wifi_handler = NULL;
    s_wifi_user_data = NULL;
    s_wifi_deinit_calls++;
    mock_unlock();
}

int mybot_device_lifecycle_init(const char *server_base, const char *device_id,
                                const char *firmware_ver, const char *hw_model,
                                mybot_device_lifecycle_callbacks_t *callbacks) {
    assert(strcmp(server_base, TEST_SERVER_BASE) == 0);
    assert(strcmp(device_id, "device-1") == 0);
    (void)firmware_ver;
    (void)hw_model;
    assert(callbacks != NULL);
    mock_lock();
    s_device_callbacks = *callbacks;
    s_device_state = MYBOT_DEVICE_STATE_RUNTIME;
    s_device_network_available = true;
    s_device_init_calls++;
    mock_unlock();
    return 0;
}

void mybot_device_lifecycle_tick(void) {
    mybot_device_lifecycle_callbacks_t callbacks;
    bool start_conversation = false;
    mock_lock();
    if (s_device_start_requested && s_device_network_available &&
        s_device_state == MYBOT_DEVICE_STATE_RUNTIME) {
        s_device_start_requested = false;
        s_device_state = MYBOT_DEVICE_STATE_IN_CONVERSATION;
        callbacks = s_device_callbacks;
        start_conversation = true;
    }
    mock_unlock();

    if (!start_conversation) {
        return;
    }
    if (callbacks.on_state_changed) {
        callbacks.on_state_changed(MYBOT_DEVICE_STATE_IN_CONVERSATION);
    }
    mybot_conversation_params_t params;
    memset(&params, 0, sizeof(params));
    snprintf(params.conversation_id, sizeof(params.conversation_id), "%s", "conversation-1");
    snprintf(params.rtc_app_id, sizeof(params.rtc_app_id), "%s", "rtc-app");
    snprintf(params.rtc_channel, sizeof(params.rtc_channel), "%s", "rtc-channel");
    snprintf(params.rtc_uid, sizeof(params.rtc_uid), "%s", "device-uid");
    snprintf(params.rtc_token, sizeof(params.rtc_token), "%s", "rtc-token");
    callbacks.on_conversation_start(&params);
}

void mybot_device_lifecycle_set_network_available(bool available) {
    mock_lock();
    s_device_network_available = available;
    if (available) {
        s_network_up_calls++;
    } else {
        s_network_down_calls++;
    }
    mock_unlock();
}

void mybot_device_lifecycle_shutdown(void) {
    mybot_device_lifecycle_callbacks_t callbacks;
    bool stop_conversation = false;
    mock_lock();
    s_device_shutdown_calls++;
    if (s_device_state == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        s_device_state = MYBOT_DEVICE_STATE_RUNTIME;
        callbacks = s_device_callbacks;
        stop_conversation = true;
    }
    mock_unlock();
    if (stop_conversation && callbacks.on_conversation_stop) {
        callbacks.on_conversation_stop();
    }
}

mybot_device_state_t mybot_device_lifecycle_get_state(void) {
    mock_lock();
    mybot_device_state_t state = s_device_state;
    mock_unlock();
    return state;
}

void mybot_device_lifecycle_request_pair(void) {
}

void mybot_device_lifecycle_request_start(void) {
    mock_lock();
    s_device_start_requested = true;
    mock_unlock();
}

void mybot_device_lifecycle_request_stop(void) {
}

void mybot_device_lifecycle_notify_conversation_ended(void) {
    mock_lock();
    s_device_state = MYBOT_DEVICE_STATE_RUNTIME;
    mock_unlock();
}

void mybot_device_lifecycle_request_rtc_token_renewal(void) {
    mock_lock();
    s_token_renewal_requests++;
    mock_unlock();
}

int mybot_rtc_session_init(const char *app_id, mybot_rtc_session_callbacks_t *callbacks) {
    assert(strcmp(app_id, "rtc-app") == 0);
    assert(callbacks != NULL);
    mock_lock();
    s_rtc_callbacks = *callbacks;
    s_rtc_initialized = true;
    s_rtc_init_calls++;
    mock_unlock();
    /* Mirror the real Agora SDK's independent AOSL ownership. */
    aosl_ctor();
    return 0;
}

int mybot_rtc_session_join(const char *channel, const char *token, const char *user_account) {
    assert(strcmp(token, "rtc-token") == 0);
    mock_lock();
    assert(s_rtc_initialized);
    snprintf(s_join_channel, sizeof(s_join_channel), "%s", channel);
    snprintf(s_join_user, sizeof(s_join_user), "%s", user_account);
    s_rtc_joined = true;
    s_rtc_join_calls++;
    void (*callback)(mybot_rtc_state_t) = s_rtc_callbacks.on_state_changed;
    mock_unlock();
    if (callback) {
        callback(MYBOT_RTC_STATE_CONNECTED);
    }
    return 0;
}

int mybot_rtc_session_leave(void) {
    mock_lock();
    bool was_joined = s_rtc_joined;
    s_rtc_joined = false;
    if (was_joined) {
        s_rtc_leave_calls++;
    }
    void (*callback)(mybot_rtc_state_t) = s_rtc_callbacks.on_state_changed;
    mock_unlock();
    if (was_joined && callback) {
        callback(MYBOT_RTC_STATE_INITIALIZED);
    }
    return 0;
}

void mybot_rtc_session_fini(void) {
    mock_lock();
    if (!s_rtc_initialized) {
        mock_unlock();
        return;
    }
    assert(!s_rtc_joined);
    s_rtc_initialized = false;
    s_rtc_fini_calls++;
    mock_unlock();
    /* Release the mock SDK's independent AOSL ownership. */
    aosl_dtor();
}

int mybot_rtc_session_send_audio(const void *data, size_t len) {
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

int mybot_rtc_session_renew_token(const char *token) {
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

int main(void) {
    mybot_config_t config;
    memset(&config, 0, sizeof(config));
    snprintf(config.server_base, sizeof(config.server_base), "%s", TEST_SERVER_BASE);
    snprintf(config.device_id, sizeof(config.device_id), "%s", "device-1");
    snprintf(config.firmware_ver, sizeof(config.firmware_ver), "%s", "test-fw");
    snprintf(config.hw_model, sizeof(config.hw_model), "%s", "test-hw");

    assert(mybot_get_state() == MYBOT_STATE_STOPPED);
    assert(mybot_start(&config) == 0);
    assert(mybot_is_running());
    assert(mybot_get_state() == MYBOT_STATE_WIFI_PROVISIONING);
    assert(read_counter(&s_wifi_init_calls) == 1);

    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_READY, 3000));
    assert(read_counter(&s_kv_init_calls) == 1);
    assert(read_counter(&s_key_init_calls) == 1);
    assert(read_counter(&s_capture_init_calls) == 1);
    assert(read_counter(&s_capture_start_calls) == 1);
    assert(read_counter(&s_playback_init_calls) == 1);
    assert(read_counter(&s_playback_start_calls) == 1);
    assert(read_counter(&s_device_init_calls) == 1);

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
    emit_wifi_event(MYBOT_WIFI_EVENT_FAILED);
    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
    assert(read_counter(&s_network_down_calls) == 2);
    assert(read_counter(&s_network_up_calls) == 2);

    mybot_app_start_conversation();
    assert(wait_for_counter(&s_rtc_join_calls, 1, 3000));
    mock_lock();
    assert(strcmp(s_join_channel, "rtc-channel") == 0);
    assert(strcmp(s_join_user, "device-uid") == 0);
    mock_unlock();

    mock_lock();
    void (*token_will_expire)(void) = s_rtc_callbacks.on_token_will_expire;
    int (*token_renewed)(const char *) = s_device_callbacks.on_rtc_token_renewed;
    mock_unlock();
    assert(token_will_expire != NULL);
    assert(token_renewed != NULL);

    token_will_expire();
    assert(read_counter(&s_token_renewal_requests) == 1);
    assert(token_renewed("renewed-token") == 0);
    assert(read_counter(&s_rtc_renew_calls) == 1);
    mock_lock();
    assert(strcmp(s_renewed_token, "renewed-token") == 0);
    mock_unlock();

    assert(wait_for_counter(&s_rtc_send_calls, 1, 3000));
    assert(wait_for_audio_frame(0, 1000));

#if MYBOT_CLOUD_AEC
    int16_t reference_frame[TEST_FRAME_SAMPLES];
    for (int i = 0; i < TEST_FRAME_SAMPLES; i++) {
        reference_frame[i] = TEST_REF_SAMPLE;
    }
    emit_remote_audio(reference_frame, sizeof(reference_frame));
    assert(wait_for_counter(&s_playback_write_calls, 1, 1000));
    assert(wait_for_audio_frame(TEST_REF_SAMPLE, 2000));
#endif

    mybot_stop();
    assert(!mybot_is_running());
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);
    assert(read_counter(&s_device_shutdown_calls) == 1);
    assert(read_counter(&s_rtc_init_calls) == 1);
    assert(read_counter(&s_rtc_join_calls) == 1);
    assert(read_counter(&s_rtc_leave_calls) == 1);
    assert(read_counter(&s_rtc_fini_calls) == 1);
    assert(read_counter(&s_capture_stop_calls) == 1);
    assert(read_counter(&s_capture_destroy_calls) == 1);
    assert(read_counter(&s_playback_stop_calls) == 1);
    assert(read_counter(&s_playback_destroy_calls) == 1);
    assert(read_counter(&s_kv_deinit_calls) == 1);
    assert(read_counter(&s_key_deinit_calls) == 1);
    assert(read_counter(&s_wifi_deinit_calls) == 1);

    mybot_stop();
    assert(read_counter(&s_device_shutdown_calls) == 1);
    assert(read_counter(&s_rtc_fini_calls) == 1);

    puts("app_test: ok");
    return 0;
}
