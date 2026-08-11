/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_AUDIO_INTERNAL_H_
#define MYBOT_AUDIO_INTERNAL_H_

#include <mybot/platform/mybot_audio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * SDK-internal audio facade. The public mybot/platform/mybot_audio.h only
 * exposes the platform contract (ops tables + registration); volume control is
 * SDK-internal: a registered device volume implementation is the primary path,
 * and the media-volume software gain is the fallback when no implementation is active.
 */

/** Get the registered capture ops, or NULL if none registered. */
const mybot_audio_capture_ops_t *mybot_audio_get_capture(void);

/** Get the registered playback ops, or NULL if none registered. */
const mybot_audio_playback_ops_t *mybot_audio_get_playback(void);

/** Initialize the registered volume implementation. Call from the app startup path. */
int mybot_audio_device_volume_init(void);

/** Release the volume implementation. Idempotent. */
void mybot_audio_device_volume_deinit(void);

/** Return whether a device volume implementation is registered, regardless of active state. */
bool mybot_audio_device_volume_is_registered(void);

/** Return whether the registered device volume implementation is initialized and active. */
bool mybot_audio_device_volume_is_active(void);

/**
 * Set the real device volume through the active implementation.
 *
 * @param volume 0 (mute) .. MYBOT_AUDIO_VOLUME_MAX (full scale)
 * @return 0 on success, -1 if the implementation is unavailable or volume is invalid
 */
int mybot_audio_device_set_volume(int volume);

/**
 * Get the current real device volume.
 *
 * Reads the implementation when it provides get_volume(); otherwise returns the
 * SDK-tracked last value. Succeeds only while the implementation is active.
 *
 * @param volume [out] current real device volume
 * @return 0 on success, -1 if the implementation is unavailable or volume is NULL
 */
int mybot_audio_device_get_volume(int *volume);

/**
 * Set the fallback media volume.
 *
 * Stored as a 0..100 software gain applied to downlink PCM only when no
 * device volume implementation is active. Volume 100 is unity gain.
 *
 * @param volume 0 (mute) .. MYBOT_AUDIO_VOLUME_MAX (full scale)
 * @return 0 on success, -1 on invalid volume
 */
int mybot_audio_set_media_volume(int volume);

/** Get the fallback media volume setting (defaults to MYBOT_AUDIO_VOLUME_DEFAULT). */
int mybot_audio_get_media_volume(void);

/** Apply the current media volume to a signed 16-bit PCM buffer in place. */
void mybot_audio_apply_media_volume(int16_t *pcm, int samples);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_AUDIO_INTERNAL_H_ */
