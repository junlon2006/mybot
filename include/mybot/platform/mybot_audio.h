/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_AUDIO_H_
#define MYBOT_AUDIO_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <mybot/mybot_export.h>

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

/**
 * Capture device operations.
 *
 * The implementation provides a PCM capture device. The SDK drives it on a
 * dedicated worker thread with the fixed format 16000 Hz, mono, 16 bits per
 * sample. Byte counts never appear in this interface: read()/write() counts
 * are PCM frames (one sample per channel).
 */
typedef struct {
    /** Implementation name for logging and diagnostics. */
    const char *name;

    /**
     * Allocate and open the capture device.
     *
     * @param ctx      [out] device context handle
     * @param rate     sample rate in Hz (e.g. 16000)
     * @param channels number of channels (e.g. 1)
     * @param bits     bits per sample (e.g. 16)
     * @return 0 on success, -1 on error
     */
    int (*init)(void **ctx, int rate, int channels, int bits);

    /**
     * Start the capture stream and unblock read().
     *
     * @param ctx device context from init()
     * @return 0 on success, -1 on error
     */
    int (*start)(void *ctx);

    /**
     * Read one block of PCM frames.
     *
     * @param ctx    device context from init()
     * @param buf    destination buffer (size = frames * channels * bits/8)
     * @param frames number of frames to read
     * @return frames actually read (positive), 0 on no progress, or -1 on
     *         error. Never return more than the requested frames.
     *
     * @note A call may block, but it must return within a bounded time so the
     *       worker can observe shutdown. stop() is called after the worker exits.
     */
    int (*read)(void *ctx, void *buf, int frames);

    /**
     * Stop the capture stream.
     *
     * @param ctx device context from init()
     * @return 0 on success, -1 on error
     */
    int (*stop)(void *ctx);

    /**
     * Destroy and close the device.
     *
     * Called only after the SDK worker thread has stopped.
     *
     * @param ctx device context from init()
     */
    void (*destroy)(void *ctx);
} mybot_audio_capture_ops_t;

/**
 * Playback device operations.
 *
 * The implementation provides a PCM playback device with the same fixed format
 * as capture (16000 Hz, mono, 16 bits per sample) and the same
 * frame-count convention.
 */
typedef struct {
    /** Implementation name for logging and diagnostics. */
    const char *name;

    /**
     * Allocate and open the playback device.
     *
     * @param ctx      [out] device context handle
     * @param rate     sample rate in Hz (e.g. 16000)
     * @param channels number of channels (e.g. 1)
     * @param bits     bits per sample (e.g. 16)
     * @return 0 on success, -1 on error
     */
    int (*init)(void **ctx, int rate, int channels, int bits);

    /**
     * Start the playback stream.
     *
     * @param ctx device context from init()
     * @return 0 on success, -1 on error
     */
    int (*start)(void *ctx);

    /**
     * Write one block of PCM frames.
     *
     * @param ctx    device context from init()
     * @param buf    source buffer (size = frames * channels * bits/8)
     * @param frames number of frames to write
     * @return frames actually written (positive), 0 on no progress, or -1 on
     *         error. Never return more than the requested frames.
     *
     * @note A call may block, but it must return within a bounded time so the
     *       worker can observe shutdown. stop() is called after the worker exits.
     */
    int (*write)(void *ctx, const void *buf, int frames);

    /**
     * Stop the playback stream.
     *
     * @param ctx device context from init()
     * @return 0 on success, -1 on error
     */
    int (*stop)(void *ctx);

    /**
     * Destroy and close the device.
     *
     * Called only after the SDK worker thread has stopped.
     *
     * @param ctx device context from init()
     */
    void (*destroy)(void *ctx);
} mybot_audio_playback_ops_t;

/* ----------------------------------------------------------
 * Device volume operations (optional hook interface)
 *
 * The SDK owns volume control; there is no application-facing volume API.
 * When a device volume implementation is registered, the SDK drives real hardware
 * (codec / amplifier / mixer) through set_volume()/get_volume(). Without a
 * registered implementation the SDK falls back to a software media gain applied to
 * the playback PCM stream.
 * ---------------------------------------------------------- */

/**
 * Shared volume range for both media volume and real device volume.
 *
 * 0 is mute; 100 is full scale (unity gain for media volume).
 */
#define MYBOT_AUDIO_VOLUME_MIN 0
#define MYBOT_AUDIO_VOLUME_MAX 100
#define MYBOT_AUDIO_VOLUME_DEFAULT MYBOT_AUDIO_VOLUME_MAX

/**
 * Real device volume operations (optional implementation).
 *
 * The implementation owns the hardware volume control (codec register, amplifier or
 * mixer). init, set_volume and destroy are required; get_volume is optional
 * and may be NULL. When no implementation is registered the SDK simply disables
 * device volume control — playback and media volume keep working.
 */
typedef struct {
    /** Implementation name for logging and diagnostics. */
    const char *name;

    /**
     * Allocate and open the hardware volume control.
     *
     * @param ctx [out] volume control context handle
     * @return 0 on success, -1 on error
     */
    int (*init)(void **ctx);

    /**
     * Set the real device volume.
     *
     * @param ctx    volume control context from init()
     * @param volume 0 (mute) .. MYBOT_AUDIO_VOLUME_MAX (full scale)
     * @return 0 on success, -1 on error
     */
    int (*set_volume)(void *ctx, int volume);

    /**
     * Get the current real device volume. Optional; may be NULL.
     *
     * @param ctx    volume control context from init()
     * @param volume [out] current volume, 0 .. MYBOT_AUDIO_VOLUME_MAX
     * @return 0 on success, -1 on error
     */
    int (*get_volume)(void *ctx, int *volume);

    /**
     * Release the hardware volume control.
     *
     * @param ctx volume control context from init()
     */
    void (*destroy)(void *ctx);
} mybot_audio_volume_ops_t;

/* ----------------------------------------------------------
 * Registration API — called by platform implementations
 * ---------------------------------------------------------- */

/**
 * Register the complete capture device ops.
 *
 * @param ops capture operations table; must remain valid for the process
 *            lifetime
 * @return 0 on success, -1 if ops is invalid or already registered
 *
 * @note Call exactly once, before mybot_start().
 */
MYBOT_API int mybot_audio_register_capture(const mybot_audio_capture_ops_t *ops);

/**
 * Register the complete playback device ops.
 *
 * @param ops playback operations table; must remain valid for the process
 *            lifetime
 * @return 0 on success, -1 if ops is invalid or already registered
 *
 * @note Call exactly once, before mybot_start().
 */
MYBOT_API int mybot_audio_register_playback(const mybot_audio_playback_ops_t *ops);

/**
 * Register the real device volume implementation.
 *
 * @param ops volume operations table; must remain valid for the process
 *            lifetime
 * @return 0 on success, -1 if ops is invalid or already registered
 *
 * @note Call exactly once, before mybot_start(). The SDK initializes the
 *       implementation during startup and drives it internally; no application code
 *       calls the volume control functions.
 */
MYBOT_API int mybot_audio_device_register_volume(const mybot_audio_volume_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_AUDIO_H_ */
