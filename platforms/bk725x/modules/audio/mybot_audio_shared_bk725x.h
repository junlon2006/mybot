/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_AUDIO_SHARED_BK725X_H_
#define MYBOT_AUDIO_SHARED_BK725X_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Shared playback (single pipeline for all audio sources) ---- */

/* Start the shared playback infrastructure (power, pipeline, ringbuf).
 * Idempotent: safe to call when already started. After this call the
 * prompt player and SDK can write audio through the shared path without
 * creating independent pipelines. */
int mybot_audio_bk725x_shared_playback_start(void);

/* Write PCM frames directly to the shared playback pipeline.
 * Returns frames written, 0 when the pipeline is full, -1 on error. */
int mybot_audio_bk725x_shared_playback_write(const int16_t *pcm, int frames);

/* Return the shared pipeline context for use with the platform
 * playback ops write path. */
void *mybot_audio_bk725x_shared_playback_get_context(void);

/* Return true when the shared pipeline is running. */
bool mybot_audio_bk725x_shared_playback_is_started(void);

/* Stop the shared playback infrastructure and release the pipeline. */
void mybot_audio_bk725x_shared_playback_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_AUDIO_SHARED_BK725X_H_ */
