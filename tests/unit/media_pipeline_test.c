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

static int capture_init(void **ctx, int rate, int channels, int bits) {
    assert(rate == MYBOT_MEDIA_SAMPLE_RATE && channels == MYBOT_MEDIA_CHANNELS && bits == 16);
    *ctx = &s_capture_ctx;
    return 0;
}
static int capture_start(void *ctx) {
    return ctx == &s_capture_ctx ? 0 : -1;
}
static int capture_read(void *ctx, void *buf, int frames) {
    (void)buf;
    (void)frames;
    return ctx == &s_capture_ctx ? 0 : -1;
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
    aosl_atomic_set(&s_last_sample, ((const int16_t *)buf)[0]);
    aosl_atomic_inc(&s_playback_writes);
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

static int send_audio(const void *data, size_t len, void *user_data) {
    (void)data;
    (void)len;
    (void)user_data;
    return 0;
}

int main(void) {
    aosl_ctor();
    aosl_atomic_set(&s_playback_writes, 0);
    aosl_atomic_set(&s_last_sample, 0);

    mybot_media_pipeline_t pipeline;
    mybot_media_pipeline_init(&pipeline);
    mybot_media_pipeline_callbacks_t callbacks = {.send_audio = send_audio};
    assert(mybot_platform_registry_get()->audio_capture == &s_capture_ops);
    assert(mybot_platform_registry_get()->audio_playback == &s_playback_ops);
    mybot_audio_t audio_probe;
    mybot_audio_context_init(&audio_probe);
    assert(audio_probe.capture_ops == &s_capture_ops);
    assert(audio_probe.playback_ops == &s_playback_ops);
    assert(mybot_media_pipeline_start(&pipeline, &callbacks) == 0);
    mybot_media_pipeline_set_rtc_connected(&pipeline, true);

    int16_t rtc_frame[MYBOT_MEDIA_FRAME_SAMPLES];
    for (int i = 0; i < MYBOT_MEDIA_FRAME_SAMPLES; ++i) {
        rtc_frame[i] = 7;
    }
    mybot_media_pipeline_push_remote_audio(&pipeline, rtc_frame, sizeof(rtc_frame));
    assert(wait_for_writes(1, 1000));
    assert(aosl_atomic_read(&s_last_sample) == 7);

    int writes_before_prompt = (int)aosl_atomic_read(&s_playback_writes);
    assert(mybot_media_pipeline_play_prompt(&pipeline, MYBOT_PROMPT_PAIR_CODE, "1") == 0);
    assert(wait_for_writes(writes_before_prompt + 1, 1000));
    assert(aosl_atomic_read(&s_last_sample) == 100);
    mybot_media_pipeline_stop_prompt(&pipeline);

    mybot_media_pipeline_set_rtc_connected(&pipeline, false);
    assert(mybot_media_pipeline_flush_session(&pipeline) == 0);
    assert(mybot_media_pipeline_stop(&pipeline) == 0);
    assert(mybot_media_pipeline_destroy(&pipeline) == 0);
    aosl_dtor();
    puts("media_pipeline_test: ok");
    return 0;
}
