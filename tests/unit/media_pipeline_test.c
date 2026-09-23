/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_media_pipeline.h"
#include "mybot_platform_registry.h"

#include <api/aosl.h>
#include <api/aosl_atomic.h>
#include <hal/aosl_hal_time.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int s_capture_ctx;
static int s_playback_ctx;
static aosl_atomic_t s_playback_writes;
static aosl_atomic_t s_last_sample;
static aosl_atomic_t s_send_calls;
static aosl_atomic_t s_last_send_len;
static int s_capture_mode;
static int s_playback_mode;
#if MYBOT_WAKE_WORDS
static mybot_wake_words_handler_t s_wake_handler;
static void *s_wake_user_data;
static aosl_atomic_t s_wake_calls;

static int wake_init(void **ctx, int rate, int channels, int bits,
                     mybot_wake_words_handler_t handler, void *user_data) {
    assert(rate == MYBOT_MEDIA_SAMPLE_RATE && channels == MYBOT_MEDIA_CHANNELS && bits == 16);
    assert(handler != NULL);
    *ctx = &s_wake_calls;
    s_wake_handler = handler;
    s_wake_user_data = user_data;
    return 0;
}

static int wake_process(void *ctx, const void *pcm, int frames) {
    assert(ctx == &s_wake_calls && pcm != NULL && frames > 0);
    s_wake_handler("hello", s_wake_user_data);
    return 0;
}

static void wake_destroy(void *ctx) {
    assert(ctx == &s_wake_calls);
}

static void on_wake_word(const char *word, void *user_data) {
    (void)user_data;
    assert(strcmp(word, "hello") == 0);
    aosl_atomic_inc(&s_wake_calls);
}

static const mybot_wake_words_ops_t s_wake_ops = {
    .init = wake_init,
    .process = wake_process,
    .destroy = wake_destroy,
};
#endif

enum {
    CAPTURE_MODE_NONE = 0,
    CAPTURE_MODE_FRAME,
    CAPTURE_MODE_INVALID,
};

enum {
    PLAYBACK_MODE_FULL = 0,
    PLAYBACK_MODE_SHORT,
    PLAYBACK_MODE_ZERO,
    PLAYBACK_MODE_FAIL,
};

static int capture_init(void **ctx, int rate, int channels, int bits) {
    assert(rate == MYBOT_MEDIA_SAMPLE_RATE && channels == MYBOT_MEDIA_CHANNELS && bits == 16);
    *ctx = &s_capture_ctx;
    return 0;
}
static int capture_start(void *ctx) {
    return ctx == &s_capture_ctx ? 0 : -1;
}
static int capture_read(void *ctx, void *buf, int frames) {
    if (ctx != &s_capture_ctx) {
        return -1;
    }
    if (s_capture_mode == CAPTURE_MODE_INVALID) {
        return frames + 1;
    }
    if (s_capture_mode != CAPTURE_MODE_FRAME) {
        return 0;
    }
    int16_t *pcm = buf;
    for (int i = 0; i < frames; ++i) {
        pcm[i] = (int16_t)(100 + i);
    }
    return frames;
}
static int capture_stop(void *ctx) {
    return ctx == &s_capture_ctx ? 0 : -1;
}
static void capture_destroy(void *ctx) {
    assert(ctx == &s_capture_ctx);
}

static int playback_init(void **ctx, int rate, int channels, int bits) {
    assert(rate == MYBOT_MEDIA_SAMPLE_RATE && channels == MYBOT_MEDIA_CHANNELS && bits == 16);
    *ctx = &s_playback_ctx;
    return 0;
}
static int playback_start(void *ctx) {
    return ctx == &s_playback_ctx ? 0 : -1;
}
static int playback_write(void *ctx, const void *buf, int frames) {
    assert(ctx == &s_playback_ctx && buf != NULL && frames > 0);
    aosl_atomic_inc(&s_playback_writes);
    if (s_playback_mode == PLAYBACK_MODE_ZERO) {
        return 0;
    }
    if (s_playback_mode == PLAYBACK_MODE_FAIL) {
        return -1;
    }
    aosl_atomic_set(&s_last_sample, ((const int16_t *)buf)[0]);
    if (s_playback_mode == PLAYBACK_MODE_SHORT) {
        return frames > 1 ? frames / 2 : 1;
    }
    return frames;
}
static int playback_stop(void *ctx) {
    return ctx == &s_playback_ctx ? 0 : -1;
}
static void playback_destroy(void *ctx) {
    assert(ctx == &s_playback_ctx);
}

static int announce_init(void **ctx) {
    *ctx = &s_capture_ctx;
    return 0;
}
static void *announce_open(void *ctx, mybot_announce_sound_t sound) {
    (void)ctx;
    return (void *)(uintptr_t)(sound + 1);
}
static int announce_read(void *ctx, void *sound, int16_t *dst, int max_frames) {
    (void)ctx;
    assert(sound != NULL && dst != NULL && max_frames > 0);
    for (int i = 0; i < max_frames; ++i) {
        dst[i] = (int16_t)((uintptr_t)sound * 100);
    }
    return max_frames;
}
static void announce_close(void *ctx, void *sound) {
    (void)ctx;
    (void)sound;
}
static void announce_destroy(void *ctx) {
    assert(ctx == &s_capture_ctx);
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
static const mybot_announce_ops_t s_announce_ops = {
    .init = announce_init,
    .open = announce_open,
    .read = announce_read,
    .close = announce_close,
    .destroy = announce_destroy,
};
static const mybot_platform_descriptor_t s_platform = {
    .audio_capture = &s_capture_ops,
    .audio_playback = &s_playback_ops,
    .announce = &s_announce_ops,
#if MYBOT_WAKE_WORDS
    .wake_words = &s_wake_ops,
#endif
};

bool mybot_platform_registry_is_registered(void) {
    return true;
}
const mybot_platform_descriptor_t *mybot_platform_registry_get(void) {
    return &s_platform;
}

static bool wait_for_writes(int minimum, int timeout_ms) {
    for (int elapsed = 0; elapsed < timeout_ms; ++elapsed) {
        if (aosl_atomic_read(&s_playback_writes) >= minimum) {
            return true;
        }
        aosl_hal_msleep(1);
    }
    return false;
}

static bool wait_for_sends(int minimum, int timeout_ms) {
    for (int elapsed = 0; elapsed < timeout_ms; ++elapsed) {
        if (aosl_atomic_read(&s_send_calls) >= minimum) {
            return true;
        }
        aosl_hal_msleep(1);
    }
    return false;
}

static int send_audio(const void *data, size_t len, void *user_data) {
    assert(data != NULL && len > 0);
    (void)user_data;
    aosl_atomic_set(&s_last_send_len, (intptr_t)len);
    aosl_atomic_inc(&s_send_calls);
    return 0;
}

int main(void) {
    mybot_media_pipeline_init(NULL);
    assert(mybot_media_pipeline_stop(NULL) < 0);
    assert(mybot_media_pipeline_destroy(NULL) < 0);

    aosl_ctor();
    aosl_atomic_set(&s_playback_writes, 0);
    aosl_atomic_set(&s_last_sample, 0);
    aosl_atomic_set(&s_send_calls, 0);
    aosl_atomic_set(&s_last_send_len, 0);

    mybot_media_pipeline_t pipeline;
    mybot_media_pipeline_init(&pipeline);
    mybot_media_pipeline_callbacks_t callbacks = {.send_audio = send_audio};
#if MYBOT_WAKE_WORDS
    callbacks.on_wake_word = on_wake_word;
#endif
    assert(mybot_media_pipeline_start(&pipeline, NULL) < 0);
    mybot_media_pipeline_callbacks_t invalid_callbacks = {0};
    assert(mybot_media_pipeline_start(&pipeline, &invalid_callbacks) < 0);
    assert(mybot_media_pipeline_destroy(&pipeline) < 0);
    assert(mybot_platform_registry_get()->audio_capture == &s_capture_ops);
    assert(mybot_platform_registry_get()->audio_playback == &s_playback_ops);
    mybot_audio_t audio_probe;
    mybot_audio_context_init(&audio_probe);
    assert(audio_probe.capture_ops == &s_capture_ops);
    assert(audio_probe.playback_ops == &s_playback_ops);
    assert(mybot_media_pipeline_start(&pipeline, &callbacks) == 0);
#if MYBOT_WAKE_WORDS
    mybot_media_pipeline_set_wake_words_enabled(&pipeline, true);
#endif
    mybot_media_pipeline_set_rtc_connected(&pipeline, true);

    int16_t rtc_frame[MYBOT_MEDIA_FRAME_SAMPLES];
    for (int i = 0; i < MYBOT_MEDIA_FRAME_SAMPLES; ++i) {
        rtc_frame[i] = 7;
    }
    mybot_media_pipeline_push_remote_audio(NULL, rtc_frame, sizeof(rtc_frame));
    mybot_media_pipeline_push_remote_audio(&pipeline, NULL, sizeof(rtc_frame));
    mybot_media_pipeline_push_remote_audio(&pipeline, rtc_frame, 0);
    mybot_media_pipeline_push_remote_audio(&pipeline, rtc_frame, sizeof(rtc_frame));
    assert(wait_for_writes(1, 1000));
    assert(aosl_atomic_read(&s_last_sample) == 7);

    /* A short write keeps the pending frame and resumes with the remainder. */
    s_playback_mode = PLAYBACK_MODE_SHORT;
    int writes_before_short = (int)aosl_atomic_read(&s_playback_writes);
    rtc_frame[0] = 9;
    mybot_media_pipeline_push_remote_audio(&pipeline, rtc_frame, sizeof(rtc_frame));
    assert(wait_for_writes(writes_before_short + 1, 1000));
    s_playback_mode = PLAYBACK_MODE_FULL;
    assert(wait_for_writes(writes_before_short + 2, 1000));

    /* Zero progress is retried without losing the pending frame. */
    s_playback_mode = PLAYBACK_MODE_ZERO;
    int writes_before_zero = (int)aosl_atomic_read(&s_playback_writes);
    mybot_media_pipeline_push_remote_audio(&pipeline, rtc_frame, sizeof(rtc_frame));
    assert(wait_for_writes(writes_before_zero + 1, 1000));
    s_playback_mode = PLAYBACK_MODE_FULL;
    assert(wait_for_writes(writes_before_zero + 2, 1000));

    /* A failed write drops only the current pending frame; later audio still works. */
    s_playback_mode = PLAYBACK_MODE_FAIL;
    int writes_before_fail = (int)aosl_atomic_read(&s_playback_writes);
    mybot_media_pipeline_push_remote_audio(&pipeline, rtc_frame, sizeof(rtc_frame));
    assert(wait_for_writes(writes_before_fail + 1, 1000));
    s_playback_mode = PLAYBACK_MODE_FULL;
    mybot_media_pipeline_push_remote_audio(&pipeline, rtc_frame, sizeof(rtc_frame));
    assert(wait_for_writes(writes_before_fail + 2, 1000));

    int writes_before_prompt = (int)aosl_atomic_read(&s_playback_writes);
    assert(mybot_media_pipeline_play_prompt(&pipeline, MYBOT_PROMPT_PAIR_CODE, "1") == 0);
    assert(wait_for_writes(writes_before_prompt + 1, 1000));
    assert(aosl_atomic_read(&s_last_sample) == 100);
    mybot_media_pipeline_stop_prompt(&pipeline);

    /* Capture and uplink run only when the capture source produces frames. */
    s_capture_mode = CAPTURE_MODE_INVALID;
    aosl_hal_msleep(70);
    s_capture_mode = CAPTURE_MODE_FRAME;
    assert(wait_for_sends(1, 1000));
#if MYBOT_WAKE_WORDS
    assert(aosl_atomic_read(&s_wake_calls) > 0);
#endif
    assert(aosl_atomic_read(&s_last_send_len) > 0);
    mybot_media_pipeline_adjust_volume(&pipeline, -20);
    assert(mybot_audio_get_media_volume(&pipeline.audio) == 80);
    mybot_media_pipeline_adjust_volume(&pipeline, 30);
    assert(mybot_audio_get_media_volume(&pipeline.audio) == 100);

    mybot_media_pipeline_set_rtc_connected(&pipeline, false);
    mybot_media_pipeline_push_remote_audio(&pipeline, rtc_frame, sizeof(rtc_frame));
    assert(mybot_media_pipeline_flush_session(&pipeline) == 0);
    assert(mybot_media_pipeline_stop(&pipeline) == 0);
    assert(mybot_media_pipeline_destroy(&pipeline) == 0);
    aosl_dtor();
    puts("media_pipeline_test: ok");
    return 0;
}
