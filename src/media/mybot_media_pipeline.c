/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_media_pipeline.h"

#include <mybot/mybot_build_config.h>
#include <mybot/platform/mybot_audio.h>

#include <api/aosl_log.h>

#include <string.h>

#define MEDIA_FRAME_DURATION_MS MYBOT_AUDIO_PTIME_MS
#define MEDIA_RINGBUF_DURATION_MS 2000
#define MEDIA_RINGBUF_SIZE                                                                         \
    (MYBOT_MEDIA_SAMPLE_RATE * MEDIA_RINGBUF_DURATION_MS / 1000 * MYBOT_MEDIA_CHANNELS *           \
     (MYBOT_MEDIA_BITS_PER_SAMPLE / 8))
#define MEDIA_ANNOUNCE_BUFFER_MS 500
#define MEDIA_ANNOUNCE_TARGET_BYTES                                                                \
    (MYBOT_MEDIA_SAMPLE_RATE * MEDIA_ANNOUNCE_BUFFER_MS / 1000 * MYBOT_MEDIA_CHANNELS *            \
     (MYBOT_MEDIA_BITS_PER_SAMPLE / 8))
#define MEDIA_MPQ_STACK_SIZE 16384

static void capture_timer(aosl_timer_t id, const aosl_ts_t *now, uintptr_t argc, uintptr_t argv[]) {
    (void)id;
    (void)now;
    if (argc != 1) {
        return;
    }

    mybot_media_pipeline_t *pipeline = (mybot_media_pipeline_t *)argv[0];
    if (!aosl_atomic_read(&pipeline->running)) {
        return;
    }

    const mybot_audio_capture_ops_t *ops = pipeline->audio.capture_ops;
    int frames = ops->read(pipeline->cap_ctx, pipeline->cap_frame, MYBOT_MEDIA_FRAME_SAMPLES);
    if (frames <= 0) {
        return;
    }
    if (frames > MYBOT_MEDIA_FRAME_SAMPLES) {
        AOSL_LOG_ERR("capture implementation returned invalid frame count: %d > %d", frames,
                     MYBOT_MEDIA_FRAME_SAMPLES);
        return;
    }

#if MYBOT_WAKE_WORDS
    if (pipeline->wake_words.active && aosl_atomic_read(&pipeline->wake_words_enabled)) {
        if (mybot_wake_words_process(&pipeline->wake_words, pipeline->cap_frame, frames) < 0) {
            pipeline->wake_words_process_error_count++;
            if (pipeline->wake_words_process_error_count == 1 ||
                pipeline->wake_words_process_error_count % 100 == 0) {
                AOSL_LOG_WRN("wake words processing failed (%u errors)",
                             pipeline->wake_words_process_error_count);
            }
        } else {
            pipeline->wake_words_process_error_count = 0;
        }
    }
#endif

    if (!aosl_atomic_read(&pipeline->rtc_connected)) {
        return;
    }

    int bytes_read = frames * MYBOT_MEDIA_CHANNELS * (MYBOT_MEDIA_BITS_PER_SAMPLE / 8);
    if (mybot_ringbuf_write(pipeline->cap_ringbuf, (char *)pipeline->cap_frame, bytes_read) < 0) {
        if (++pipeline->cap_drop_count % 100 == 0) {
            AOSL_LOG_WRN("cap ringbuf full, dropped %d", pipeline->cap_drop_count);
        }
    }
}

static int capture_worker_init(void *arg) {
    mybot_media_pipeline_t *pipeline = arg;
    pipeline->cap_timer =
        aosl_mpq_set_timer(MEDIA_FRAME_DURATION_MS, capture_timer, NULL, 1, (uintptr_t)pipeline);
    return aosl_mpq_timer_invalid(pipeline->cap_timer) ? -1 : 0;
}

static void capture_worker_fini(void *arg) {
    mybot_media_pipeline_t *pipeline = arg;
    if (!aosl_mpq_timer_invalid(pipeline->cap_timer)) {
        aosl_mpq_kill_timer(pipeline->cap_timer);
        pipeline->cap_timer = AOSL_MPQ_TIMER_INVALID;
    }
}

static void playback_timer(aosl_timer_t id, const aosl_ts_t *now, uintptr_t argc,
                           uintptr_t argv[]) {
    (void)id;
    (void)now;
    if (argc != 1) {
        return;
    }

    mybot_media_pipeline_t *pipeline = (mybot_media_pipeline_t *)argv[0];
    if (!aosl_atomic_read(&pipeline->running)) {
        return;
    }

    if (aosl_atomic_cmpxchg(&pipeline->announce_clear_pb, true, false)) {
        mybot_ringbuf_clear(pipeline->pb_ringbuf);
        pipeline->pb_pending_offset = 0;
        pipeline->pb_pending_frames = 0;
    }

    const mybot_audio_playback_ops_t *ops = pipeline->audio.playback_ops;
    while (mybot_announce_is_active(&pipeline->announce) &&
           mybot_ringbuf_get_data_size(pipeline->pb_ringbuf) < MEDIA_ANNOUNCE_TARGET_BYTES &&
           mybot_ringbuf_get_free_size(pipeline->pb_ringbuf) >= MYBOT_MEDIA_FRAME_BYTES) {
        int frames = mybot_announce_read_pcm(&pipeline->announce, pipeline->announce_frame,
                                             MYBOT_MEDIA_FRAME_SAMPLES);
        if (frames <= 0) {
            break;
        }
        if (mybot_ringbuf_write(pipeline->pb_ringbuf, (const char *)pipeline->announce_frame,
                                frames * MYBOT_MEDIA_CHANNELS * (MYBOT_MEDIA_BITS_PER_SAMPLE / 8)) <
            0) {
            AOSL_LOG_WRN("pb ringbuf full while feeding announcement");
        }
    }

    if (pipeline->pb_pending_frames == 0) {
        if (mybot_ringbuf_get_data_size(pipeline->pb_ringbuf) < MYBOT_MEDIA_FRAME_BYTES) {
            return;
        }
        if (mybot_ringbuf_read((char *)pipeline->pb_pending, MYBOT_MEDIA_FRAME_BYTES,
                               pipeline->pb_ringbuf) != MYBOT_MEDIA_FRAME_BYTES) {
            return;
        }
        pipeline->pb_pending_offset = 0;
        pipeline->pb_pending_frames = MYBOT_MEDIA_FRAME_SAMPLES;

        if (!mybot_audio_device_volume_is_active(&pipeline->audio)) {
            mybot_audio_apply_media_volume(&pipeline->audio, pipeline->pb_pending,
                                           MYBOT_MEDIA_FRAME_SAMPLES * MYBOT_MEDIA_CHANNELS);
        }

#if MYBOT_CLOUD_AEC
        if (aosl_atomic_read(&pipeline->rtc_connected)) {
            mybot_ringbuf_write(pipeline->ref_ringbuf, (const char *)pipeline->pb_pending,
                                MYBOT_MEDIA_FRAME_BYTES);
        }
#endif
    }

    int frame_bytes = MYBOT_MEDIA_CHANNELS * (MYBOT_MEDIA_BITS_PER_SAMPLE / 8);
    int written =
        ops->write(pipeline->pb_ctx,
                   (const char *)pipeline->pb_pending + pipeline->pb_pending_offset * frame_bytes,
                   pipeline->pb_pending_frames);
    if (written < 0 || written > pipeline->pb_pending_frames) {
        AOSL_LOG_ERR("playback write failed, dropping %d pending frames",
                     pipeline->pb_pending_frames);
        pipeline->pb_pending_offset = 0;
        pipeline->pb_pending_frames = 0;
        return;
    }
    if (written == 0) {
        return;
    }

    pipeline->pb_pending_offset += written;
    pipeline->pb_pending_frames -= written;
    if (pipeline->pb_pending_frames == 0) {
        pipeline->pb_pending_offset = 0;
    }
}

static int playback_worker_init(void *arg) {
    mybot_media_pipeline_t *pipeline = arg;
    pipeline->pb_timer =
        aosl_mpq_set_timer(MEDIA_FRAME_DURATION_MS, playback_timer, NULL, 1, (uintptr_t)pipeline);
    return aosl_mpq_timer_invalid(pipeline->pb_timer) ? -1 : 0;
}

static void playback_worker_fini(void *arg) {
    mybot_media_pipeline_t *pipeline = arg;
    if (!aosl_mpq_timer_invalid(pipeline->pb_timer)) {
        aosl_mpq_kill_timer(pipeline->pb_timer);
        pipeline->pb_timer = AOSL_MPQ_TIMER_INVALID;
    }
}

static void send_timer(aosl_timer_t id, const aosl_ts_t *now, uintptr_t argc, uintptr_t argv[]) {
    (void)id;
    (void)now;
    if (argc != 1) {
        return;
    }

    mybot_media_pipeline_t *pipeline = (mybot_media_pipeline_t *)argv[0];
    if (!aosl_atomic_read(&pipeline->rtc_connected) || !pipeline->cbs.send_audio ||
        mybot_ringbuf_get_data_size(pipeline->cap_ringbuf) < MYBOT_MEDIA_FRAME_BYTES) {
        return;
    }

    if (mybot_ringbuf_read((char *)pipeline->send_frame, MYBOT_MEDIA_FRAME_BYTES,
                           pipeline->cap_ringbuf) != MYBOT_MEDIA_FRAME_BYTES) {
        return;
    }

#if MYBOT_CLOUD_AEC
    memset(pipeline->aec_reference_frame, 0, sizeof(pipeline->aec_reference_frame));
    if (mybot_ringbuf_get_data_size(pipeline->ref_ringbuf) >= MYBOT_MEDIA_FRAME_BYTES) {
        mybot_ringbuf_read((char *)pipeline->aec_reference_frame, MYBOT_MEDIA_FRAME_BYTES,
                           pipeline->ref_ringbuf);
    }
    for (size_t i = 0; i < MYBOT_MEDIA_FRAME_SAMPLES; i++) {
        pipeline->aec_interleaved_frame[i * 2] = pipeline->send_frame[i];
        pipeline->aec_interleaved_frame[i * 2 + 1] = pipeline->aec_reference_frame[i];
    }
    pipeline->cbs.send_audio(pipeline->aec_interleaved_frame,
                             sizeof(pipeline->aec_interleaved_frame), pipeline->cbs.user_data);
#else
    pipeline->cbs.send_audio(pipeline->send_frame, MYBOT_MEDIA_FRAME_BYTES,
                             pipeline->cbs.user_data);
#endif
}

static int send_worker_init(void *arg) {
    mybot_media_pipeline_t *pipeline = arg;
    pipeline->send_timer =
        aosl_mpq_set_timer(MEDIA_FRAME_DURATION_MS, send_timer, NULL, 1, (uintptr_t)pipeline);
    return aosl_mpq_timer_invalid(pipeline->send_timer) ? -1 : 0;
}

static void send_worker_fini(void *arg) {
    mybot_media_pipeline_t *pipeline = arg;
    if (!aosl_mpq_timer_invalid(pipeline->send_timer)) {
        aosl_mpq_kill_timer(pipeline->send_timer);
        pipeline->send_timer = AOSL_MPQ_TIMER_INVALID;
    }
}

static void init_handles(mybot_media_pipeline_t *pipeline) {
    pipeline->cap_mpq = AOSL_MPQ_INVALID;
    pipeline->cap_timer = AOSL_MPQ_TIMER_INVALID;
    pipeline->pb_mpq = AOSL_MPQ_INVALID;
    pipeline->pb_timer = AOSL_MPQ_TIMER_INVALID;
    pipeline->send_mpq = AOSL_MPQ_INVALID;
    pipeline->send_timer = AOSL_MPQ_TIMER_INVALID;
}

int mybot_media_pipeline_start(mybot_media_pipeline_t *pipeline,
                               const mybot_media_pipeline_callbacks_t *callbacks) {
    if (!pipeline || !callbacks || !callbacks->send_audio) {
        return -1;
    }

    memset(pipeline, 0, sizeof(*pipeline));
    pipeline->cbs = *callbacks;
    init_handles(pipeline);
    mybot_audio_context_init(&pipeline->audio);
    aosl_atomic_set(&pipeline->running, true);

    const mybot_audio_capture_ops_t *cap_ops = pipeline->audio.capture_ops;
    const mybot_audio_playback_ops_t *pb_ops = pipeline->audio.playback_ops;
    if (!cap_ops || !pb_ops) {
        AOSL_LOG_ERR("no audio platform registered");
        goto fail;
    }

    if (mybot_announce_init(&pipeline->announce) < 0) {
        AOSL_LOG_WRN("announce init failed, pairing voice prompt disabled");
    }
    if (cap_ops->init(&pipeline->cap_ctx, MYBOT_MEDIA_SAMPLE_RATE, MYBOT_MEDIA_CHANNELS,
                      MYBOT_MEDIA_BITS_PER_SAMPLE) < 0 ||
        pb_ops->init(&pipeline->pb_ctx, MYBOT_MEDIA_SAMPLE_RATE, MYBOT_MEDIA_CHANNELS,
                     MYBOT_MEDIA_BITS_PER_SAMPLE) < 0) {
        goto fail;
    }

    if (mybot_audio_device_volume_init(&pipeline->audio) < 0) {
        AOSL_LOG_WRN("device volume unavailable, using software media volume");
    }

#if MYBOT_WAKE_WORDS
    if (mybot_wake_words_init(&pipeline->wake_words, MYBOT_MEDIA_SAMPLE_RATE, MYBOT_MEDIA_CHANNELS,
                              MYBOT_MEDIA_BITS_PER_SAMPLE, callbacks->on_wake_word,
                              callbacks->user_data) < 0) {
        AOSL_LOG_ERR("wake words enabled but implementation initialization failed");
        goto fail;
    }
#endif

    pipeline->cap_ringbuf = mybot_ringbuf_create(MEDIA_RINGBUF_SIZE);
    pipeline->pb_ringbuf = mybot_ringbuf_create(MEDIA_RINGBUF_SIZE);
    if (!pipeline->cap_ringbuf || !pipeline->pb_ringbuf) {
        goto fail;
    }
#if MYBOT_CLOUD_AEC
    pipeline->ref_ringbuf = mybot_ringbuf_create(MEDIA_RINGBUF_SIZE);
    if (!pipeline->ref_ringbuf) {
        goto fail;
    }
#endif

    if (cap_ops->start(pipeline->cap_ctx) < 0) {
        goto fail;
    }
    pipeline->cap_started = true;
    if (pb_ops->start(pipeline->pb_ctx) < 0) {
        goto fail;
    }
    pipeline->pb_started = true;

    pipeline->cap_mpq = aosl_mpq_create(AOSL_THRD_PRI_NORMAL, MEDIA_MPQ_STACK_SIZE, 1000, "cap_mpq",
                                        capture_worker_init, capture_worker_fini, pipeline);
    pipeline->pb_mpq = aosl_mpq_create(AOSL_THRD_PRI_NORMAL, MEDIA_MPQ_STACK_SIZE, 1000, "pb_mpq",
                                       playback_worker_init, playback_worker_fini, pipeline);
    pipeline->send_mpq = aosl_mpq_create(AOSL_THRD_PRI_NORMAL, MEDIA_MPQ_STACK_SIZE, 1000,
                                         "mybot_mpq", send_worker_init, send_worker_fini, pipeline);
    if (aosl_mpq_invalid(pipeline->cap_mpq) || aosl_mpq_invalid(pipeline->pb_mpq) ||
        aosl_mpq_invalid(pipeline->send_mpq)) {
        goto fail;
    }
    return 0;

fail:
    mybot_media_pipeline_stop(pipeline);
    mybot_media_pipeline_destroy(pipeline);
    return -1;
}

void mybot_media_pipeline_stop(mybot_media_pipeline_t *pipeline) {
    if (!pipeline) {
        return;
    }
    aosl_atomic_set(&pipeline->running, false);

    const mybot_audio_capture_ops_t *cap_ops = pipeline->audio.capture_ops;
    const mybot_audio_playback_ops_t *pb_ops = pipeline->audio.playback_ops;

    /* Interrupt both device directions before waiting for any worker. A platform read/write may
     * be blocked inside its worker and relies on stop() being called from this shutdown thread. */
    if (pipeline->cap_started && cap_ops) {
        (void)cap_ops->stop(pipeline->cap_ctx);
        pipeline->cap_started = false;
    }
    if (pipeline->pb_started && pb_ops) {
        (void)pb_ops->stop(pipeline->pb_ctx);
        pipeline->pb_started = false;
    }

    if (!aosl_mpq_invalid(pipeline->send_mpq)) {
        aosl_mpq_destroy_wait(pipeline->send_mpq);
        pipeline->send_mpq = AOSL_MPQ_INVALID;
    }
    if (!aosl_mpq_invalid(pipeline->cap_mpq)) {
        aosl_mpq_destroy_wait(pipeline->cap_mpq);
        pipeline->cap_mpq = AOSL_MPQ_INVALID;
    }
    if (!aosl_mpq_invalid(pipeline->pb_mpq)) {
        aosl_mpq_destroy_wait(pipeline->pb_mpq);
        pipeline->pb_mpq = AOSL_MPQ_INVALID;
    }

    mybot_announce_deinit(&pipeline->announce);
#if MYBOT_WAKE_WORDS
    mybot_wake_words_deinit(&pipeline->wake_words);
#endif

    if (pipeline->cap_ctx && cap_ops) {
        cap_ops->destroy(pipeline->cap_ctx);
        pipeline->cap_ctx = NULL;
    }
    if (pipeline->pb_ctx && pb_ops) {
        pb_ops->destroy(pipeline->pb_ctx);
        pipeline->pb_ctx = NULL;
    }
    mybot_audio_device_volume_deinit(&pipeline->audio);
}

void mybot_media_pipeline_destroy(mybot_media_pipeline_t *pipeline) {
    if (!pipeline) {
        return;
    }
    if (pipeline->cap_ringbuf) {
        mybot_ringbuf_destroy(pipeline->cap_ringbuf);
        pipeline->cap_ringbuf = NULL;
    }
    if (pipeline->pb_ringbuf) {
        mybot_ringbuf_destroy(pipeline->pb_ringbuf);
        pipeline->pb_ringbuf = NULL;
    }
#if MYBOT_CLOUD_AEC
    if (pipeline->ref_ringbuf) {
        mybot_ringbuf_destroy(pipeline->ref_ringbuf);
        pipeline->ref_ringbuf = NULL;
    }
#endif
}

void mybot_media_pipeline_set_rtc_connected(mybot_media_pipeline_t *pipeline, bool connected) {
    if (pipeline) {
        aosl_atomic_set(&pipeline->rtc_connected, connected);
    }
}

#if MYBOT_WAKE_WORDS
void mybot_media_pipeline_set_wake_words_enabled(mybot_media_pipeline_t *pipeline, bool enabled) {
    if (pipeline) {
        aosl_atomic_set(&pipeline->wake_words_enabled, enabled);
    }
}
#endif

void mybot_media_pipeline_push_remote_audio(mybot_media_pipeline_t *pipeline, const void *data,
                                            size_t len) {
    if (!pipeline || !data || len == 0 || !aosl_atomic_read(&pipeline->running) ||
        mybot_announce_is_active(&pipeline->announce)) {
        return;
    }
    if (mybot_ringbuf_write(pipeline->pb_ringbuf, (const char *)data, (int)len) < 0) {
        AOSL_LOG_WRN("pb ringbuf full, dropped");
    }
}

void mybot_media_pipeline_play_pair_code(mybot_media_pipeline_t *pipeline, const char *code) {
    if (pipeline) {
        (void)mybot_announce_play_pair_code(&pipeline->announce, code);
    }
}

void mybot_media_pipeline_stop_announcement(mybot_media_pipeline_t *pipeline) {
    if (!pipeline) {
        return;
    }
    mybot_announce_stop(&pipeline->announce);
    aosl_atomic_set(&pipeline->announce_clear_pb, true);
}

void mybot_media_pipeline_adjust_volume(mybot_media_pipeline_t *pipeline, int delta) {
    if (!pipeline) {
        return;
    }

    int volume;
    if (mybot_audio_device_volume_is_active(&pipeline->audio)) {
        if (mybot_audio_device_get_volume(&pipeline->audio, &volume) == 0) {
            volume += delta;
            if (volume < MYBOT_AUDIO_VOLUME_MIN) {
                volume = MYBOT_AUDIO_VOLUME_MIN;
            } else if (volume > MYBOT_AUDIO_VOLUME_MAX) {
                volume = MYBOT_AUDIO_VOLUME_MAX;
            }
            (void)mybot_audio_device_set_volume(&pipeline->audio, volume);
        }
        return;
    }

    volume = mybot_audio_get_media_volume(&pipeline->audio) + delta;
    if (volume < MYBOT_AUDIO_VOLUME_MIN) {
        volume = MYBOT_AUDIO_VOLUME_MIN;
    } else if (volume > MYBOT_AUDIO_VOLUME_MAX) {
        volume = MYBOT_AUDIO_VOLUME_MAX;
    }
    (void)mybot_audio_set_media_volume(&pipeline->audio, volume);
}
