/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_AUDIO_INTERNAL_H_
#define MYBOT_AUDIO_INTERNAL_H_

#include <mybot/platform/mybot_audio.h>

#include <api/aosl_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const mybot_audio_capture_ops_t *capture_ops;
    const mybot_audio_playback_ops_t *playback_ops;
    const mybot_audio_volume_ops_t *volume_ops;
    void *volume_ctx;
    bool volume_active;
    int device_volume;
    aosl_atomic_t media_volume;
} mybot_audio_t;

/**
 * SDK-internal audio facade. The public mybot/platform/mybot_audio.h only
 * exposes the platform contract (ops tables + registration); volume control is
 * SDK-internal: a registered device volume implementation is the primary path,
 * and the media-volume software gain is the fallback when no implementation is active.
 */

/** Snapshot the default registered implementations into a runtime context. */
void mybot_audio_context_init(mybot_audio_t *audio);

/** Initialize the registered volume implementation. Call from the app startup path. */
int mybot_audio_device_volume_init(mybot_audio_t *audio);

/** Release the volume implementation. Idempotent. */
void mybot_audio_device_volume_deinit(mybot_audio_t *audio);

/** Return whether the registered device volume implementation is initialized and active. */
bool mybot_audio_device_volume_is_active(const mybot_audio_t *audio);

/**
 * Set the real device volume through the active implementation.
 *
 * @param volume 0 (mute) .. MYBOT_AUDIO_VOLUME_MAX (full scale)
 * @return 0 on success, -1 if the implementation is unavailable or volume is invalid
 */
int mybot_audio_device_set_volume(mybot_audio_t *audio, int volume);

/**
 * Get the current real device volume.
 *
 * Reads the implementation when it provides get_volume(); otherwise returns the
 * SDK-tracked last value. Succeeds only while the implementation is active.
 *
 * @param volume [out] current real device volume
 * @return 0 on success, -1 if the implementation is unavailable or volume is NULL
 */
int mybot_audio_device_get_volume(mybot_audio_t *audio, int *volume);

/**
 * Set the fallback media volume.
 *
 * Stored as a 0..100 software gain applied to downlink PCM only when no
 * device volume implementation is active. Volume 100 is unity gain.
 *
 * @param volume 0 (mute) .. MYBOT_AUDIO_VOLUME_MAX (full scale)
 * @return 0 on success, -1 on invalid volume
 */
int mybot_audio_set_media_volume(mybot_audio_t *audio, int volume);

/** Get the fallback media volume setting (defaults to MYBOT_AUDIO_VOLUME_DEFAULT). */
int mybot_audio_get_media_volume(const mybot_audio_t *audio);

/** Apply the current media volume to a signed 16-bit PCM buffer in place. */
void mybot_audio_apply_media_volume(const mybot_audio_t *audio, int16_t *pcm, int samples);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_AUDIO_INTERNAL_H_ */
