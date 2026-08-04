#ifndef MYBOT_AUDIO_DEVICE_H_
#define MYBOT_AUDIO_DEVICE_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------
 * Platform audio device operations (hook interface)
 *
 * Each platform provides one capture ops and one playback ops.
 * The framework registers them at startup and uses the
 * unified audio_device_* API everywhere.
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
    int  (*init)(void **ctx, int rate, int channels, int bits);

    /** Start capture stream (unblock capture_read). */
    int  (*start)(void *ctx);

    /** Read one PCM frame (blocking).
     *  @param buf    destination buffer (size = frames * channels * bits/8)
     *  @param frames number of frames to read
     *  @return frames actually read, or -1 on error.
     */
    int  (*read)(void *ctx, void *buf, int frames);

    /** Stop capture stream. */
    int  (*stop)(void *ctx);

    /** Destroy and close the device. */
    void (*destroy)(void *ctx);
} mybot_audio_capture_ops_t;

/** Playback device operations */
typedef struct {
    const char *name;

    /** Allocate and open the playback device. */
    int  (*init)(void **ctx, int rate, int channels, int bits);

    /** Start playback stream. */
    int  (*start)(void *ctx);

    /** Write one PCM frame (blocking). */
    int  (*write)(void *ctx, const void *buf, int frames);

    /** Stop playback stream. */
    int  (*stop)(void *ctx);

    /** Destroy and close the device. */
    void (*destroy)(void *ctx);
} mybot_audio_playback_ops_t;

/* ----------------------------------------------------------
 * Registration API — called by platform implementations
 * ---------------------------------------------------------- */

/** Register capture device ops (call once at startup). */
int mybot_audio_device_register_capture(const mybot_audio_capture_ops_t *ops);

/** Register playback device ops (call once at startup). */
int mybot_audio_device_register_playback(const mybot_audio_playback_ops_t *ops);

/* ----------------------------------------------------------
 * Unified access API — called by application code
 * ---------------------------------------------------------- */

/** Get the registered capture ops, or NULL if none registered. */
const mybot_audio_capture_ops_t *mybot_audio_device_get_capture(void);

/** Get the registered playback ops, or NULL if none registered. */
const mybot_audio_playback_ops_t *mybot_audio_device_get_playback(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_AUDIO_DEVICE_H_ */
