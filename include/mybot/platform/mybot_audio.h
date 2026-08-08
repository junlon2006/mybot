/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_AUDIO_H_
#define MYBOT_AUDIO_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------
 * Platform audio device operations (hook interface)
 *
 * Each platform provides one capture ops and one playback ops.
 * The framework registers them at startup and uses the
 * unified mybot_audio_* API everywhere.
 * ---------------------------------------------------------- */

/** Capture device operations */
typedef struct {
    const char *name;

    /** Allocate and open the capture device.
     *  @param ctx       [out] device context handle
     *  @param rate      sample rate in Hz (e.g. 16000)
     *  @param channels  number of channels (e.g. 1)
     *  @param bits      bits per sample (e.g. 16)
     *  @return 0 on success, -1 on error.
     */
    int (*init)(void **ctx, int rate, int channels, int bits);

    /** Start capture stream (unblock capture_read). */
    int (*start)(void *ctx);

    /** Read one PCM frame (blocking).
     *  @param buf    destination buffer (size = frames * channels * bits/8)
     *  @param frames number of frames to read
     *  @return frames actually read, or -1 on error.
     */
    int (*read)(void *ctx, void *buf, int frames);

    /** Stop capture stream. */
    int (*stop)(void *ctx);

    /** Destroy and close the device. */
    void (*destroy)(void *ctx);
} mybot_audio_capture_ops_t;

/** Playback device operations */
typedef struct {
    const char *name;

    /** Allocate and open the playback device. */
    int (*init)(void **ctx, int rate, int channels, int bits);

    /** Start playback stream. */
    int (*start)(void *ctx);

    /** Write one PCM frame (blocking). */
    int (*write)(void *ctx, const void *buf, int frames);

    /** Stop playback stream. */
    int (*stop)(void *ctx);

    /** Destroy and close the device. */
    void (*destroy)(void *ctx);
} mybot_audio_playback_ops_t;

/* ----------------------------------------------------------
 * Registration API — called by platform implementations
 * ---------------------------------------------------------- */

/** Register complete capture device ops once, before mybot_app_start(). */
int mybot_audio_register_capture(const mybot_audio_capture_ops_t *ops);

/** Register complete playback device ops once, before mybot_app_start(). */
int mybot_audio_register_playback(const mybot_audio_playback_ops_t *ops);

/** Shared volume range for both media volume and real device volume. */
#define MYBOT_AUDIO_VOLUME_MIN 0
#define MYBOT_AUDIO_VOLUME_MAX 100
#define MYBOT_AUDIO_VOLUME_DEFAULT MYBOT_AUDIO_VOLUME_MAX

/* ----------------------------------------------------------
 * Device volume operations (optional hook interface)
 *
 * The real hardware volume is owned by the platform backend and reaches the
 * actual codec / amplifier / mixer through set_volume() and get_volume().
 * It is independent from the SDK-managed media volume, which is applied as a
 * digital software gain on the playback PCM stream.
 * ---------------------------------------------------------- */

/** Real device volume operations (optional backend). */
typedef struct {
    const char *name;

    /** Allocate and open the hardware volume control.
     *  @param ctx [out] volume control context handle
     *  @return 0 on success, -1 on error.
     */
    int (*init)(void **ctx);

    /** Set the real device volume.
     *  @param volume 0 (mute) .. MYBOT_AUDIO_VOLUME_MAX (full scale)
     *  @return 0 on success, -1 on error.
     */
    int (*set_volume)(void *ctx, int volume);

    /** Get the current real device volume. Optional; may be NULL.
     *  @return 0 on success, -1 on error.
     */
    int (*get_volume)(void *ctx, int *volume);

    /** Release the hardware volume control. */
    void (*destroy)(void *ctx);
} mybot_audio_volume_ops_t;

/** Register the real device volume backend once, before mybot_app_start(). */
int mybot_audio_device_register_volume(const mybot_audio_volume_ops_t *ops);

/** Initialize the registered volume backend. Call from the app startup path. */
int mybot_audio_device_volume_init(void);

/** Release the volume backend. Idempotent. */
void mybot_audio_device_volume_deinit(void);

/* ----------------------------------------------------------
 * Unified access API — called by application code
 * ---------------------------------------------------------- */

/** Get the registered capture ops, or NULL if none registered. */
const mybot_audio_capture_ops_t *mybot_audio_get_capture(void);

/** Get the registered playback ops, or NULL if none registered. */
const mybot_audio_playback_ops_t *mybot_audio_get_playback(void);

/** Return whether a real device volume backend is registered. */
bool mybot_audio_device_volume_is_registered(void);

/** Set the real device volume through the registered backend.
 *  @return 0 on success, -1 if the backend is unavailable or volume is invalid.
 */
int mybot_audio_device_set_volume(int volume);

/** Get the real device volume through the registered backend.
 *  @return 0 on success, -1 if the backend is unavailable.
 */
int mybot_audio_device_get_volume(int *volume);

/* ----------------------------------------------------------
 * Media volume — SDK-managed digital software gain
 *
 * The SDK scales downlink PCM in software before it reaches the playback
 * device, so media volume works on every platform without any backend.
 * ---------------------------------------------------------- */

/** Set the media volume, 0 (mute) .. MYBOT_AUDIO_VOLUME_MAX (full scale).
 *  Volume 100 is unity gain and skips processing entirely.
 *  @return 0 on success, -1 on invalid volume.
 */
int mybot_audio_set_media_volume(int volume);

/** Get the current media volume setting. */
int mybot_audio_get_media_volume(void);

/** Apply the current media volume to a signed 16-bit PCM buffer in place.
 *  The SDK playback pipeline calls this automatically before writing to the
 *  device; it is exposed for testing and advanced use.
 *  @param pcm     buffer of interleaved int16 samples
 *  @param samples total number of samples (frames * channels)
 */
void mybot_audio_apply_media_volume(int16_t *pcm, int samples);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_AUDIO_H_ */
