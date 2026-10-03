/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_media_pipeline.h"
#include "mybot_platform_registry.h"

#include <api/aosl.h>
#include <api/aosl_atomic.h>
#include <hal/aosl_hal_time.h>

#include <assert.h>
#include <pthread.h>
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
static bool s_capture_init_fail;
static int s_announce_init_calls;
static int s_announce_destroy_calls;
static aosl_atomic_t s_prompt_fixture;
static aosl_atomic_t s_prompt_asset_sample;
static aosl_atomic_t s_block_prompt_open;
static aosl_atomic_t s_prompt_open_entered;
static aosl_atomic_t s_release_prompt_open;
static aosl_atomic_t s_fail_prompt_open;
static aosl_atomic_t s_prompt_write_count;
static aosl_atomic_t s_block_prompt_write;
static aosl_atomic_t s_prompt_write_entered;
static aosl_atomic_t s_release_prompt_write;
static aosl_atomic_t s_short_prompt_write;
static aosl_atomic_t s_prompt_write_result;
static aosl_atomic_t s_prompt_capture_enabled;
#if MYBOT_CLOUD_AEC
static aosl_atomic_t s_prompt_reference_seen;
static aosl_atomic_t s_prompt_reference_invalid;
#endif
static int16_t s_prompt_write_samples[64];

static bool wait_for_atomic(const aosl_atomic_t *value, intptr_t expected) {
    for (int elapsed = 0; elapsed < 1000; ++elapsed) {
        if (aosl_atomic_read(value) == expected) {
            return true;
        }
        aosl_hal_msleep(1);
    }
    return false;
}
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
    if (s_capture_init_fail) {
        return -1;
    }
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
    int mode = s_capture_mode;
    if (aosl_atomic_read(&s_prompt_fixture)) {
        mode = aosl_atomic_read(&s_prompt_capture_enabled) ? CAPTURE_MODE_FRAME : CAPTURE_MODE_NONE;
    }
    if (mode == CAPTURE_MODE_INVALID) {
        return frames + 1;
    }
    if (mode != CAPTURE_MODE_FRAME) {
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
    if (aosl_atomic_read(&s_prompt_fixture)) {
        int index = (int)aosl_atomic_add_return(1, &s_prompt_write_count);
        assert(index <= (int)(sizeof(s_prompt_write_samples) / sizeof(s_prompt_write_samples[0])));
        s_prompt_write_samples[index - 1] = ((const int16_t *)buf)[0];
        if (index == aosl_atomic_read(&s_block_prompt_write)) {
            aosl_atomic_set(&s_prompt_write_entered, index);
            bool released = wait_for_atomic(&s_release_prompt_write, index);
            assert(released);
        }
        if (index == aosl_atomic_read(&s_short_prompt_write)) {
            return (int)aosl_atomic_read(&s_prompt_write_result);
        }
        return frames;
    }
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
    s_announce_init_calls++;
    *ctx = &s_capture_ctx;
    return 0;
}
static void *announce_open(void *ctx, mybot_announce_sound_t sound) {
    (void)ctx;
    if (aosl_atomic_read(&s_prompt_fixture)) {
        assert(sound == MYBOT_ANNOUNCE_SOUND_PROMPT);
        if (aosl_atomic_xchg(&s_block_prompt_open, false)) {
            aosl_atomic_set(&s_prompt_open_entered, true);
            bool released = wait_for_atomic(&s_release_prompt_open, true);
            assert(released);
        }
        if (aosl_atomic_read(&s_fail_prompt_open)) {
            return NULL;
        }
        /* Each handle keeps its PCM identity after the next asset is selected. */
        return (void *)(uintptr_t)aosl_atomic_read(&s_prompt_asset_sample);
    }
    return (void *)(uintptr_t)(sound + 1);
}
static int announce_read(void *ctx, void *sound, int16_t *dst, int max_frames) {
    (void)ctx;
    assert(sound != NULL && dst != NULL && max_frames > 0);
    for (int i = 0; i < max_frames; ++i) {
        dst[i] = (uintptr_t)sound >= 1000 ? (int16_t)((uintptr_t)sound + (uintptr_t)i)
                                          : (int16_t)((uintptr_t)sound * 100);
    }
    return max_frames;
}
static void announce_close(void *ctx, void *sound) {
    (void)ctx;
    (void)sound;
}
static void announce_destroy(void *ctx) {
    assert(ctx == &s_capture_ctx);
    s_announce_destroy_calls++;
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
#if MYBOT_CLOUD_AEC
    if (aosl_atomic_read(&s_prompt_fixture)) {
        assert(len == MYBOT_MEDIA_FRAME_BYTES * 2);
        const int16_t *pcm = data;
        bool accepted_rtc_frame = true;
        for (int i = 0; i < MYBOT_MEDIA_FRAME_SAMPLES; ++i) {
            int16_t reference = pcm[i * 2 + 1];
            if (reference != 0 && reference != 7) {
                aosl_atomic_set(&s_prompt_reference_invalid, true);
            }
            if (reference != 7) {
                accepted_rtc_frame = false;
            }
        }
        if (accepted_rtc_frame) {
            aosl_atomic_set(&s_prompt_reference_seen, true);
        }
    }
#endif
    aosl_atomic_set(&s_last_send_len, (intptr_t)len);
    aosl_atomic_inc(&s_send_calls);
    return 0;
}

typedef struct {
    mybot_media_pipeline_t *pipeline;
    int result;
} prompt_replacement_t;

static void *replace_prompt(void *arg) {
    prompt_replacement_t *replacement = arg;
    replacement->result =
        mybot_media_pipeline_play_prompt(replacement->pipeline, MYBOT_PROMPT_PAIR_CODE, "");
    return NULL;
}

static void init_prompt_fixture(mybot_media_pipeline_t *pipeline,
                                const mybot_media_pipeline_callbacks_t *callbacks, int short_write,
                                int write_result) {
    mybot_media_pipeline_init(pipeline);
    assert(mybot_media_pipeline_start(pipeline, callbacks) == 0);
    aosl_atomic_set(&s_prompt_fixture, true);
    aosl_atomic_set(&s_prompt_asset_sample, 1000);
    aosl_atomic_set(&s_block_prompt_open, false);
    aosl_atomic_set(&s_prompt_open_entered, false);
    aosl_atomic_set(&s_release_prompt_open, false);
    aosl_atomic_set(&s_fail_prompt_open, false);
    aosl_atomic_set(&s_prompt_write_count, 0);
    aosl_atomic_set(&s_block_prompt_write, 1);
    aosl_atomic_set(&s_prompt_write_entered, 0);
    aosl_atomic_set(&s_release_prompt_write, 0);
    aosl_atomic_set(&s_short_prompt_write, short_write);
    aosl_atomic_set(&s_prompt_write_result, write_result);
    aosl_atomic_set(&s_prompt_capture_enabled, false);
#if MYBOT_CLOUD_AEC
    aosl_atomic_set(&s_prompt_reference_seen, false);
    aosl_atomic_set(&s_prompt_reference_invalid, false);
#endif
}

static void test_prompt_replacement(const mybot_media_pipeline_callbacks_t *callbacks,
                                    bool fail_load, int write_result) {
    mybot_media_pipeline_t pipeline;
    init_prompt_fixture(&pipeline, callbacks, fail_load ? 1 : 2, write_result);
    assert(mybot_media_pipeline_play_prompt(&pipeline, MYBOT_PROMPT_PAIR_CODE, "") == 0);
    assert(wait_for_atomic(&s_prompt_write_entered, 1));
    assert(s_prompt_write_samples[0] == 1000);
    uint32_t generation_before = mybot_announce_get_generation(&pipeline.announce);

    aosl_atomic_set(&s_prompt_asset_sample, 2000);
    aosl_atomic_set(&s_block_prompt_open, true);
    aosl_atomic_set(&s_fail_prompt_open, fail_load);
    prompt_replacement_t replacement = {.pipeline = &pipeline, .result = -2};
    pthread_t loader;
    int ret = pthread_create(&loader, NULL, replace_prompt, &replacement);
    assert(ret == 0);
    assert(wait_for_atomic(&s_prompt_open_entered, true));
    assert(mybot_announce_get_generation(&pipeline.announce) == generation_before);

    if (!fail_load) {
        /* Loading lasts until playback has buffered another frame from the old handle. */
        aosl_atomic_set(&s_block_prompt_write, 2);
        aosl_atomic_set(&s_release_prompt_write, 1);
        assert(wait_for_atomic(&s_prompt_write_entered, 2));
        assert(s_prompt_write_samples[1] == 1000);
    }
    aosl_atomic_set(&s_release_prompt_open, true);
    ret = pthread_join(loader, NULL);
    assert(ret == 0);
    assert(replacement.result == (fail_load ? -1 : 0));
    uint32_t generation_after = mybot_announce_get_generation(&pipeline.announce);
    assert(fail_load ? generation_after == generation_before
                     : generation_after != generation_before);

    int old_write = fail_load ? 1 : 2;
    int next_write = old_write + 1;
    aosl_atomic_set(&s_block_prompt_write, next_write);
    aosl_atomic_set(&s_release_prompt_write, old_write);
    assert(wait_for_atomic(&s_prompt_write_entered, next_write));
    /* Failed loading preserves the short-write remainder; a swap discards it. */
    assert(s_prompt_write_samples[next_write - 1] == (fail_load ? 1000 + write_result : 2000));
    aosl_atomic_set(&s_block_prompt_write, 0);
    aosl_atomic_set(&s_release_prompt_write, next_write);
    assert(mybot_media_pipeline_stop(&pipeline) == 0);
    assert(mybot_media_pipeline_destroy(&pipeline) == 0);
    aosl_atomic_set(&s_prompt_fixture, false);
}

static void *flush_prompt_session(void *arg) {
    prompt_replacement_t *flush = arg;
    flush->result = mybot_media_pipeline_flush_session(flush->pipeline);
    return NULL;
}

static void test_stop_prompt_during_write(const mybot_media_pipeline_callbacks_t *callbacks,
                                          int write_result) {
    mybot_media_pipeline_t pipeline;
    init_prompt_fixture(&pipeline, callbacks, 1, write_result);
    mybot_media_pipeline_set_rtc_connected(&pipeline, true);
    assert(mybot_media_pipeline_play_prompt(&pipeline, MYBOT_PROMPT_PAIR_CODE, "") == 0);
    assert(wait_for_atomic(&s_prompt_write_entered, 1));
    mybot_media_pipeline_stop_prompt(&pipeline);
    prompt_replacement_t flush = {.pipeline = &pipeline, .result = -2};
    pthread_t flusher;
    int ret = pthread_create(&flusher, NULL, flush_prompt_session, &flush);
    assert(ret == 0);
    bool flush_queued = false;
    for (int elapsed = 0; elapsed < 1000; ++elapsed) {
        if (aosl_mpq_queued_count(pipeline.pb_mpq) > 0) {
            flush_queued = true;
            break;
        }
        aosl_hal_msleep(1);
    }
    assert(flush_queued);
    aosl_atomic_set(&s_block_prompt_write, 2);
    aosl_atomic_set(&s_release_prompt_write, 1);
    ret = pthread_join(flusher, NULL);
    assert(ret == 0 && flush.result == 0);
    int16_t rtc_frame[MYBOT_MEDIA_FRAME_SAMPLES];
    for (int i = 0; i < MYBOT_MEDIA_FRAME_SAMPLES; ++i) {
        rtc_frame[i] = 7;
    }
    mybot_media_pipeline_push_remote_audio(&pipeline, rtc_frame, sizeof(rtc_frame));
    assert(wait_for_atomic(&s_prompt_write_entered, 2));
    assert(s_prompt_write_samples[1] == 7);
    aosl_atomic_set(&s_block_prompt_write, 0);
    aosl_atomic_set(&s_release_prompt_write, 2);
    assert(mybot_media_pipeline_stop(&pipeline) == 0);
    assert(mybot_media_pipeline_destroy(&pipeline) == 0);
    aosl_atomic_set(&s_prompt_fixture, false);
}

static void observe_playback_generation(const aosl_ts_t *ts, aosl_refobj_t ref, uintptr_t argc,
                                        uintptr_t argv[]) {
    (void)ts;
    (void)ref;
    assert(argc == 2);
    mybot_media_pipeline_t *pipeline = (mybot_media_pipeline_t *)argv[0];
    aosl_atomic_set((aosl_atomic_t *)argv[1], pipeline->pb_pending_generation);
}

static bool wait_for_playback_generation(mybot_media_pipeline_t *pipeline) {
    aosl_atomic_t observed = 0;
    uint32_t expected = mybot_announce_get_generation(&pipeline->announce);
    for (int elapsed = 0; elapsed < 1000; ++elapsed) {
        int ret = aosl_mpq_call(pipeline->pb_mpq, AOSL_REF_INVALID, "prompt_test_generation",
                                observe_playback_generation, 2, (uintptr_t)pipeline,
                                (uintptr_t)&observed);
        assert(ret == 0);
        if ((uint32_t)aosl_atomic_read(&observed) == expected) {
            return true;
        }
        aosl_hal_msleep(1);
    }
    return false;
}

static void test_prompt_aec_reference(const mybot_media_pipeline_callbacks_t *callbacks) {
    mybot_media_pipeline_t pipeline;
    init_prompt_fixture(&pipeline, callbacks, 1, MYBOT_MEDIA_FRAME_SAMPLES / 2);
    mybot_media_pipeline_set_rtc_connected(&pipeline, true);
    int16_t rtc_frame[MYBOT_MEDIA_FRAME_SAMPLES];
    for (int i = 0; i < MYBOT_MEDIA_FRAME_SAMPLES; ++i) {
        rtc_frame[i] = 7;
    }
    mybot_media_pipeline_push_remote_audio(&pipeline, rtc_frame, sizeof(rtc_frame));
    assert(wait_for_atomic(&s_prompt_write_entered, 1));
    assert(s_prompt_write_samples[0] == 7);
    assert(mybot_media_pipeline_play_prompt(&pipeline, MYBOT_PROMPT_PAIR_CODE, "") == 0);
    aosl_atomic_set(&s_block_prompt_write, 2);
    aosl_atomic_set(&s_release_prompt_write, 1);
    assert(wait_for_atomic(&s_prompt_write_entered, 2));
    assert(s_prompt_write_samples[1] == 1000);
    mybot_media_pipeline_stop_prompt(&pipeline);
    aosl_atomic_set(&s_block_prompt_write, 0);
    aosl_atomic_set(&s_release_prompt_write, 2);
    /* Observe the owner's completed stop handling before publishing live RTC PCM. */
    assert(wait_for_playback_generation(&pipeline));
    aosl_atomic_set(&s_block_prompt_write, 3);
    aosl_atomic_set(&s_short_prompt_write, 3);
    mybot_media_pipeline_push_remote_audio(&pipeline, rtc_frame, sizeof(rtc_frame));
    assert(wait_for_atomic(&s_prompt_write_entered, 3));
    assert(s_prompt_write_samples[2] == 7);
    aosl_atomic_set(&s_prompt_asset_sample, 2000);
    assert(mybot_media_pipeline_play_prompt(&pipeline, MYBOT_PROMPT_PAIR_CODE, "") == 0);
    aosl_atomic_set(&s_block_prompt_write, 4);
    aosl_atomic_set(&s_release_prompt_write, 3);
    assert(wait_for_atomic(&s_prompt_write_entered, 4));
    assert(s_prompt_write_samples[3] == 2000);
    int sends_before = (int)aosl_atomic_read(&s_send_calls);
    aosl_atomic_set(&s_prompt_capture_enabled, true);
    assert(wait_for_sends(sends_before + 1, 1000));
#if MYBOT_CLOUD_AEC
    assert(aosl_atomic_read(&s_prompt_reference_seen));
#endif
    aosl_atomic_set(&s_block_prompt_write, 0);
    aosl_atomic_set(&s_release_prompt_write, 4);
    assert(wait_for_sends(sends_before + 3, 1000));
    assert(mybot_media_pipeline_stop(&pipeline) == 0);
    assert(mybot_media_pipeline_destroy(&pipeline) == 0);
#if MYBOT_CLOUD_AEC
    assert(!aosl_atomic_read(&s_prompt_reference_invalid));
#endif
    aosl_atomic_set(&s_prompt_fixture, false);
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
    assert(mybot_media_pipeline_play_prompt(&pipeline, MYBOT_PROMPT_PAIR_CODE, "1") == 0);
    assert(mybot_media_pipeline_stop(&pipeline) == 0);
    /* In-flight RTC callbacks can still query the initialized prompt after workers stop. */
    assert(pipeline.announce.active && pipeline.announce.lock);
    assert(!mybot_announce_is_active(&pipeline.announce));
    assert(s_announce_init_calls == 1 && s_announce_destroy_calls == 0);
    assert(mybot_media_pipeline_stop(&pipeline) == 0);
    assert(mybot_media_pipeline_destroy(&pipeline) == 0);
    assert(!pipeline.announce.active && !pipeline.announce.lock);
    assert(s_announce_destroy_calls == 1);
    assert(mybot_media_pipeline_destroy(&pipeline) == 0);
    assert(s_announce_destroy_calls == 1);

    /* A failed startup still frees the prompt through stop followed by destroy. */
    mybot_media_pipeline_init(&pipeline);
    s_capture_init_fail = true;
    assert(mybot_media_pipeline_start(&pipeline, &callbacks) < 0);
    assert(!pipeline.announce.active && !pipeline.announce.lock);
    assert(s_announce_init_calls == 2 && s_announce_destroy_calls == 2);
    assert(mybot_media_pipeline_stop(&pipeline) == 0);
    assert(mybot_media_pipeline_destroy(&pipeline) == 0);
    assert(s_announce_destroy_calls == 2);
    s_capture_init_fail = false;
    s_capture_mode = CAPTURE_MODE_NONE;
    s_playback_mode = PLAYBACK_MODE_FULL;
    test_prompt_replacement(&callbacks, false, MYBOT_MEDIA_FRAME_SAMPLES / 2);
    test_prompt_replacement(&callbacks, false, 0);
    test_prompt_replacement(&callbacks, true, MYBOT_MEDIA_FRAME_SAMPLES / 2);
    test_stop_prompt_during_write(&callbacks, MYBOT_MEDIA_FRAME_SAMPLES / 2);
    test_stop_prompt_during_write(&callbacks, 0);
    test_prompt_aec_reference(&callbacks);
    aosl_dtor();
    puts("media_pipeline_test: ok");
    return 0;
}
