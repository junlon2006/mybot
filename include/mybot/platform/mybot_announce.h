/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_ANNOUNCE_H_
#define MYBOT_ANNOUNCE_H_

#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------
 * Platform announcement (local voice prompt) operations
 *
 * The SDK plays short local prompts on the speaker through the normal playback
 * path — for example, the fixed prompt "Please enter the pairing code in the
 * console" followed by one sound per pairing-code digit.
 *
 * The platform owns the audio assets and exposes them as raw 16 kHz mono
 * signed 16-bit PCM streams. The SDK never decodes or resamples audio; the
 * platform is responsible for matching the fixed output format.
 * ---------------------------------------------------------- */

/** Logical sounds used by the SDK. */
typedef enum {
    MYBOT_ANNOUNCE_SOUND_PROMPT = 0, /* e.g. "Please enter the pairing code in the console" */
    MYBOT_ANNOUNCE_SOUND_DIGIT_0,
    MYBOT_ANNOUNCE_SOUND_DIGIT_1,
    MYBOT_ANNOUNCE_SOUND_DIGIT_2,
    MYBOT_ANNOUNCE_SOUND_DIGIT_3,
    MYBOT_ANNOUNCE_SOUND_DIGIT_4,
    MYBOT_ANNOUNCE_SOUND_DIGIT_5,
    MYBOT_ANNOUNCE_SOUND_DIGIT_6,
    MYBOT_ANNOUNCE_SOUND_DIGIT_7,
    MYBOT_ANNOUNCE_SOUND_DIGIT_8,
    MYBOT_ANNOUNCE_SOUND_DIGIT_9,
    MYBOT_ANNOUNCE_SOUND_COUNT
} mybot_announce_sound_t;

/**
 * Announcement implementation operations.
 *
 * All PCM exchanged through this interface is 16000 Hz, mono, signed 16-bit.
 * open()/read()/close() may be called from different SDK threads, and open()
 * must not depend on the caller thread; keep read() cheap (no blocking I/O)
 * because the SDK calls it from the real-time playback worker.
 */
typedef struct {
    /** Allocate and initialize the announcement implementation.
     *  @param ctx [out] implementation context handle
     *  @return 0 on success, -1 on error */
    int (*init)(void **ctx);

    /** Open one logical sound for streaming.
     *  @return a non-NULL handle on success, or NULL when the asset is
     *          unavailable (the SDK then skips that sound gracefully). */
    void *(*open)(void *ctx, mybot_announce_sound_t sound);

    /** Read up to max_frames frames (16 kHz mono s16) from an open sound.
     *  @return frames read (0 = end of sound, -1 = error treated as end). */
    int (*read)(void *ctx, void *sound, int16_t *dst, int max_frames);

    /** Close a sound handle returned by open(). */
    void (*close)(void *ctx, void *sound);

    /** Release the implementation context. */
    void (*destroy)(void *ctx);
} mybot_announce_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_ANNOUNCE_H_ */
