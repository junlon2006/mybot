/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/mybot.h>
#include <mybot/mybot_build_config.h>
#include <mybot/platform/mybot_audio.h>
#include <mybot/platform/mybot_key.h>
#include <mybot/platform/mybot_lcd.h>
#include <mybot/platform/mybot_platform.h>
#include <mybot/platform/mybot_wake_words.h>
#include <mybot/platform/mybot_wifi.h>

#include "mybot_announce_internal.h"
#include "mybot_audio_internal.h"
#include "mybot_device_lifecycle.h"
#include "mybot_key_internal.h"
#include "mybot_kv_store_internal.h"
#include "mybot_lcd_internal.h"
#include "mybot_platform_registry.h"
#include "mybot_agora_rtc.h"
#include "mybot_wake_words_internal.h"
#include "mybot_wifi_internal.h"

#include <api/aosl.h>
#include <hal/aosl_hal_time.h>

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
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
#define TEST_MAX_SENT_FRAMES 32
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

static mybot_agora_rtc_callbacks_t s_rtc_callbacks;
static bool s_rtc_initialized;
static bool s_rtc_joined;
static bool s_platform_registered;
static bool s_https_registered;
static bool s_wake_words_registered;

static const mybot_https_ops_t s_registered_https_ops;
static const mybot_wake_words_ops_t s_registered_wake_words_ops;
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

static bool wait_for_counter(const int *counter, int minimum, int timeout_ms) {
    for (int elapsed = 0; elapsed < timeout_ms; elapsed++) {
        if (read_counter(counter) >= minimum) {
            return true;
        }
        aosl_hal_msleep(1);
    }
    return false;
}

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

static void emit_key_event(mybot_key_event_t event) {
    mock_lock();
    mybot_key_event_handler_t handler = s_key_handler;
    void *user_data = s_key_user_data;
    mock_unlock();
    assert(handler != NULL);
    handler(event, user_data);
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

bool mybot_platform_registry_is_registered(void) {
    return s_platform_registered;
}

const mybot_platform_descriptor_t *mybot_platform_registry_get(void) {
    s_registry_view.https = s_https_registered ? &s_registered_https_ops : NULL;
    s_registry_view.wake_words = s_wake_words_registered ? &s_registered_wake_words_ops : NULL;
    return &s_registry_view;
}

bool mybot_lcd_is_registered(void) {
    return false;
}

int mybot_lcd_init(mybot_lcd_t *lcd) {
    assert(lcd != NULL);
    return 0;
}

int mybot_lcd_show_screen(mybot_lcd_t *lcd, mybot_lcd_screen_t screen) {
    assert(lcd != NULL);
    (void)screen;
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
    mock_unlock();
    return 0;
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
    return 0;
}

void mybot_announce_stop(mybot_announce_t *announce) {
    assert(announce != NULL);
}

bool mybot_announce_is_active(mybot_announce_t *announce) {
    assert(announce != NULL);
    mock_lock();
    s_announce_active_checks++;
    bool active = s_announce_active;
    mock_unlock();
    return active;
}

int mybot_announce_read_pcm(mybot_announce_t *announce, int16_t *dst, int max_frames) {
    assert(announce != NULL);
    (void)dst;
    (void)max_frames;
    return 0;
}

static int capture_init(void **ctx, int rate, int channels, int bits) {
    assert(rate == TEST_SAMPLE_RATE);
    assert(channels == TEST_CHANNELS);
    assert(bits == TEST_BITS_PER_SAMPLE);
    observe_control_thread(CONTROL_OBS_CAPTURE_INIT);
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
    if (s_block_playback_write) {
        wait_for_io_stop(&s_block_playback_write, &s_playback_write_blocked, &s_playback_started,
                         &s_playback_write_timed_out);
    }
    bool started = s_playback_started;
    if (started) {
        s_playback_write_calls++;
    }
    mock_unlock();
    return started ? frames : 0;
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
    memset(audio, 0, sizeof(*audio));
    audio->capture_ops = &s_capture_ops;
    audio->playback_ops = &s_playback_ops;
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
    mock_lock();
    s_media_volume_set_calls++;
    s_last_media_volume = volume;
    mock_unlock();
    return 0;
}

int mybot_audio_get_media_volume(const mybot_audio_t *audio) {
    assert(audio != NULL);
    return MYBOT_AUDIO_VOLUME_DEFAULT;
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
    mock_lock();
    s_device_lifecycle = lifecycle;
    s_device_callbacks = *callbacks;
    s_device_state = MYBOT_DEVICE_STATE_RUNTIME;
    s_device_start_requested = false;
    s_device_stop_requested = false;
    s_device_pair_requested = false;
    s_device_token_renewal_requested = false;
    s_device_network_available = true;
    s_device_init_calls++;
    mock_unlock();
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
    } action = DEVICE_TICK_IDLE;

    mock_lock();
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
    }
    mock_unlock();

    if (action == DEVICE_TICK_IDLE) {
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
        if (callbacks.on_conversation_stop) {
            callbacks.on_conversation_stop(callbacks.user_data);
        }
        if (callbacks.on_state_changed) {
            callbacks.on_state_changed(MYBOT_DEVICE_STATE_RUNTIME, callbacks.user_data);
        }
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
    mybot_conversation_params_t params;
    memset(&params, 0, sizeof(params));
    snprintf(params.conversation_id, sizeof(params.conversation_id), "%s", "conversation-1");
    snprintf(params.rtc_app_id, sizeof(params.rtc_app_id), "%s", "rtc-app");
    snprintf(params.rtc_channel, sizeof(params.rtc_channel), "%s", "rtc-channel");
    snprintf(params.rtc_uid, sizeof(params.rtc_uid), "%s", "device-uid");
    snprintf(params.rtc_token, sizeof(params.rtc_token), "%s", "rtc-token");
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
    if (s_device_state == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        s_device_state = MYBOT_DEVICE_STATE_RUNTIME;
        callbacks = s_device_callbacks;
        stop_conversation = true;
    }
    mock_unlock();
    if (stop_conversation && callbacks.on_conversation_stop) {
        callbacks.on_conversation_stop(callbacks.user_data);
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
    mock_unlock();
}

void mybot_device_lifecycle_notify_conversation_ended(mybot_device_lifecycle_t *lifecycle) {
    assert(lifecycle == s_device_lifecycle);
    mock_lock();
    s_device_state = MYBOT_DEVICE_STATE_RUNTIME;
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

int mybot_agora_rtc_join(const char *channel, const char *token, const char *user_account) {
    if (!channel || !token || !user_account) {
        return -1;
    }
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
    void (*callback)(mybot_rtc_state_t, void *) = s_rtc_callbacks.on_state_changed;
    void *user_data = s_rtc_callbacks.user_data;
    mock_unlock();
    if (callback) {
        callback(MYBOT_RTC_STATE_CONNECTED, user_data);
    }
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

void mybot_agora_rtc_fini(void) {
    observe_control_thread(0);
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
    mock_unlock();
    mybot_stop();
    mock_lock();
    s_stop_thread_returned = true;
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

int mybot_agora_rtc_renew_token(const char *token) {
    observe_control_thread(CONTROL_OBS_RTC_RENEW);
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
    s_https_registered = true;
    s_wake_words_registered = true;

    assert(mybot_get_state() == MYBOT_STATE_STOPPED);
    assert(mybot_start(NULL) < 0);
    mybot_config_t invalid = config;
    invalid.server_base[0] = '\0';
    assert(mybot_start(&invalid) < 0);
    invalid = config;
    invalid.device_id[0] = '\0';
    assert(mybot_start(&invalid) < 0);
    invalid = config;
    memset(invalid.server_base, 'x', sizeof(invalid.server_base));
    assert(mybot_start(&invalid) < 0);
    invalid = config;
    memset(invalid.device_id, 'x', sizeof(invalid.device_id));
    assert(mybot_start(&invalid) < 0);
    invalid = config;
    memset(invalid.firmware_ver, 'x', sizeof(invalid.firmware_ver));
    assert(mybot_start(&invalid) < 0);
    invalid = config;
    memset(invalid.hw_model, 'x', sizeof(invalid.hw_model));
    assert(mybot_start(&invalid) < 0);
    invalid = config;
    snprintf(invalid.server_base, sizeof(invalid.server_base), "%s", "ftp://server");
    assert(mybot_start(&invalid) < 0);

    s_platform_registered = false;
    assert(mybot_start(&config) < 0);
    assert(!mybot_is_running());
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);
    assert(read_counter(&s_wifi_init_calls) == 0);

    s_platform_registered = true;
#if MYBOT_WAKE_WORDS
    s_wake_words_registered = false;
    assert(mybot_start(&config) < 0);
    assert(!mybot_is_running());
    assert(read_counter(&s_wifi_init_calls) == 0);
    s_wake_words_registered = true;
#endif
#if MYBOT_ENABLE_HTTPS
    s_https_registered = false;
    assert(mybot_start(&config) < 0);
    assert(!mybot_is_running());
    assert(read_counter(&s_wifi_init_calls) == 0);
    s_https_registered = true;
#endif

    begin_control_thread_tracking();
    assert(mybot_start(&config) == 0);
    assert(mybot_is_running());
    assert(mybot_get_state() == MYBOT_STATE_WIFI_PROVISIONING);
    assert(read_counter(&s_wifi_init_calls) == 1);
    assert(mybot_start(&config) < 0);

    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_READY, 3000));
    assert(read_counter(&s_kv_init_calls) == 1);
    assert(read_counter(&s_key_init_calls) == 1);
    assert(read_counter(&s_capture_init_calls) == 1);
    assert(read_counter(&s_capture_start_calls) == 1);
    assert(read_counter(&s_playback_init_calls) == 1);
    assert(read_counter(&s_playback_start_calls) == 1);
    assert(read_counter(&s_device_init_calls) == 1);

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

    emit_key_event(MYBOT_KEY_EVENT_CONVERSATION_START);
    assert(wait_for_counter(&s_rtc_join_calls, 1, 3000));
    mock_lock();
    assert(strcmp(s_join_channel, "rtc-channel") == 0);
    assert(strcmp(s_join_user, "device-uid") == 0);
    mock_unlock();
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
#endif

    emit_key_event(MYBOT_KEY_EVENT_CONVERSATION_STOP);
    assert(wait_for_counter(&s_stop_requests, 1, 1000));
    assert(wait_for_counter(&s_rtc_leave_calls, 1, 1000));
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
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

    emit_key_event(MYBOT_KEY_EVENT_EXIT);
    assert(!mybot_is_running());
    int renewals_before_stop = read_counter(&s_token_renewal_requests);
    token_will_expire(rtc_user_data);
    assert(read_counter(&s_token_renewal_requests) == renewals_before_stop);
    assert(token_renewed("too-late", device_user_data) < 0);

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
    assert(read_counter(&s_capture_destroy_calls) == 1);
    assert(read_counter(&s_playback_stop_calls) == 1);
    assert(read_counter(&s_playback_destroy_calls) == 1);
    assert(read_counter(&s_kv_deinit_calls) == 1);
    assert(read_counter(&s_key_deinit_calls) == 1);
    assert(read_counter(&s_wifi_deinit_calls) == 1);

    mybot_conversation_params_t late_params;
    memset(&late_params, 0, sizeof(late_params));
    int notifications_before_late = read_counter(&s_conversation_ended_notifications);
    s_device_callbacks.on_conversation_start(&late_params, s_device_callbacks.user_data);
    assert(read_counter(&s_conversation_ended_notifications) == notifications_before_late + 1);

    mybot_stop();
    assert(read_counter(&s_device_shutdown_calls) == 1);
    assert(read_counter(&s_rtc_fini_calls) == 1);

    s_wifi_init_fails = true;
    assert(mybot_start(&config) < 0);
    s_wifi_init_fails = false;
    assert(!mybot_is_running());
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);

    s_kv_init_fails = true;
    assert(mybot_start(&config) == 0);
    emit_wifi_event(MYBOT_WIFI_EVENT_STA_CONNECTED);
    assert(wait_for_app_state(MYBOT_STATE_FAILED, 1000));
    assert(!mybot_is_running());
    mybot_stop();
    s_kv_init_fails = false;
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);

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
    int thread_result = pthread_create(&start_tid, NULL, start_thread, &config);
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

    assert(mybot_start(&config) == 0);
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

    assert(mybot_start(&config) == 0);
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

    mock_lock();
    s_wifi_emit_connected_on_init = true;
    mock_unlock();
    assert(mybot_start(&config) == 0);
    assert(wait_for_app_state(MYBOT_STATE_READY, 1000));
    mybot_stop();
    mock_lock();
    s_wifi_emit_connected_on_init = false;
    mock_unlock();

    puts("app_test: ok");
    return 0;
}
