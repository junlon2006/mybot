/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_MEDIA_PIPELINE_H_
#define MYBOT_MEDIA_PIPELINE_H_

#include "mybot_announce_internal.h"
#include "mybot_audio_internal.h"
#include "mybot_ringbuf.h"
#include "mybot_wake_words_internal.h"

#include <api/aosl_atomic.h>
#include <api/aosl_mpq.h>
#include <api/aosl_mpq_timer.h>

#include <mybot/mybot_build_config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYBOT_MEDIA_SAMPLE_RATE 16000
#define MYBOT_MEDIA_CHANNELS 1
#define MYBOT_MEDIA_BITS_PER_SAMPLE 16
#define MYBOT_MEDIA_FRAME_SAMPLES (MYBOT_MEDIA_SAMPLE_RATE * MYBOT_AUDIO_PTIME_MS / 1000)
#define MYBOT_MEDIA_FRAME_BYTES                                                                    \
    (MYBOT_MEDIA_FRAME_SAMPLES * MYBOT_MEDIA_CHANNELS * (MYBOT_MEDIA_BITS_PER_SAMPLE / 8))

typedef struct {
    int (*send_audio)(const void *data, size_t len, void *user_data);
    void (*on_wake_word)(const char *wake_word, void *user_data);
    void *user_data;
} mybot_media_pipeline_callbacks_t;

typedef struct {
    aosl_atomic_t running;
    aosl_atomic_t rtc_connected;
#if MYBOT_WAKE_WORDS
    aosl_atomic_t wake_words_enabled;
#endif
    mybot_media_pipeline_callbacks_t cbs;

    mybot_audio_t audio;
    mybot_announce_t announce;
#if MYBOT_WAKE_WORDS
    mybot_wake_words_t wake_words;
    unsigned int wake_words_process_error_count;
#endif

    void *cap_ctx;
    bool cap_started;
    aosl_mpq_t cap_mpq;
    aosl_timer_t cap_timer;
    mybot_ringbuf_t cap_ringbuf;
    uint8_t cap_frame[MYBOT_MEDIA_FRAME_BYTES];
    int cap_drop_count;

    void *pb_ctx;
    bool pb_started;
    aosl_mpq_t pb_mpq;
    aosl_timer_t pb_timer;
    mybot_ringbuf_t pb_ringbuf;
    aosl_atomic_t announce_clear_pb;
    int16_t pb_pending[MYBOT_MEDIA_FRAME_SAMPLES * MYBOT_MEDIA_CHANNELS];
    int pb_pending_offset;
    int pb_pending_frames;
    int16_t announce_frame[MYBOT_MEDIA_FRAME_SAMPLES];

#if MYBOT_CLOUD_AEC
    mybot_ringbuf_t ref_ringbuf;
    int16_t aec_reference_frame[MYBOT_MEDIA_FRAME_SAMPLES];
    int16_t aec_interleaved_frame[MYBOT_MEDIA_FRAME_SAMPLES * 2];
#endif

    aosl_mpq_t send_mpq;
    aosl_timer_t send_timer;
    int16_t send_frame[MYBOT_MEDIA_FRAME_SAMPLES * MYBOT_MEDIA_CHANNELS];
} mybot_media_pipeline_t;

int mybot_media_pipeline_start(mybot_media_pipeline_t *pipeline,
                               const mybot_media_pipeline_callbacks_t *callbacks);
void mybot_media_pipeline_stop(mybot_media_pipeline_t *pipeline);
void mybot_media_pipeline_destroy(mybot_media_pipeline_t *pipeline);

void mybot_media_pipeline_set_rtc_connected(mybot_media_pipeline_t *pipeline, bool connected);
#if MYBOT_WAKE_WORDS
void mybot_media_pipeline_set_wake_words_enabled(mybot_media_pipeline_t *pipeline, bool enabled);
#endif
void mybot_media_pipeline_push_remote_audio(mybot_media_pipeline_t *pipeline, const void *data,
                                            size_t len);
void mybot_media_pipeline_play_pair_code(mybot_media_pipeline_t *pipeline, const char *code);
void mybot_media_pipeline_stop_announcement(mybot_media_pipeline_t *pipeline);
void mybot_media_pipeline_adjust_volume(mybot_media_pipeline_t *pipeline, int delta);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_MEDIA_PIPELINE_H_ */
