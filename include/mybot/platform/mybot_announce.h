/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_ANNOUNCE_H_
#define MYBOT_ANNOUNCE_H_

#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------
 * Platform announcement operations (hook interface)
 *
 * The SDK plays short local prompts on the speaker through the normal playback
 * path — for example, the fixed prompt "Please enter the pairing code in the
 * console" followed by one sound per pairing-code digit.
 *
 * The platform owns the audio assets and exposes them as raw 16 kHz mono
 * signed 16-bit PCM streams. The SDK never decodes or resamples audio; the
 * platform is responsible for matching the fixed output format.
 * ---------------------------------------------------------- */

/**
 * Logical sounds used by the SDK.
 */
typedef enum {
    /** Fixed prompt played before the pairing-code digits. */
    MYBOT_ANNOUNCE_SOUND_PROMPT = 0,
    /** Spoken digit zero. */
    MYBOT_ANNOUNCE_SOUND_DIGIT_0,
    /** Spoken digit one. */
    MYBOT_ANNOUNCE_SOUND_DIGIT_1,
    /** Spoken digit two. */
    MYBOT_ANNOUNCE_SOUND_DIGIT_2,
    /** Spoken digit three. */
    MYBOT_ANNOUNCE_SOUND_DIGIT_3,
    /** Spoken digit four. */
    MYBOT_ANNOUNCE_SOUND_DIGIT_4,
    /** Spoken digit five. */
    MYBOT_ANNOUNCE_SOUND_DIGIT_5,
    /** Spoken digit six. */
    MYBOT_ANNOUNCE_SOUND_DIGIT_6,
    /** Spoken digit seven. */
    MYBOT_ANNOUNCE_SOUND_DIGIT_7,
    /** Spoken digit eight. */
    MYBOT_ANNOUNCE_SOUND_DIGIT_8,
    /** Spoken digit nine. */
    MYBOT_ANNOUNCE_SOUND_DIGIT_9,
    /** Sentinel; not a valid sound. */
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
    /**
     * Allocate and initialize the announcement implementation.
     *
     * @param ctx [out] implementation context handle
     * @return 0 on success, -1 on error
     */
    int (*init)(void **ctx);

    /**
     * Open one logical sound for streaming.
     *
     * @param ctx   implementation context from init()
     * @param sound logical sound to open
     * @return a non-NULL sound handle on success, or NULL when the asset is unavailable
     *
     * @note A missing fixed prompt aborts the announcement. A missing digit
     *       sound is skipped while the remaining digits continue.
     */
    void *(*open)(void *ctx, mybot_announce_sound_t sound);

    /**
     * Read PCM frames from an open sound.
     *
     * @param ctx        implementation context from init()
     * @param sound      sound handle from open()
     * @param dst        destination buffer for 16 kHz mono signed 16-bit PCM
     * @param max_frames maximum number of PCM frames to read
     * @return frames read, 0 at the end of the sound, or -1 on error
     *
     * @note The SDK treats an error as the end of the current sound. This callback runs in the
     *       real-time playback worker and must not perform blocking I/O.
     */
    int (*read)(void *ctx, void *sound, int16_t *dst, int max_frames);

    /**
     * Close an open sound.
     *
     * @param ctx   implementation context from init()
     * @param sound sound handle from open()
     */
    void (*close)(void *ctx, void *sound);

    /**
     * Release the announcement implementation.
     *
     * @param ctx implementation context from init()
     */
    void (*destroy)(void *ctx);
} mybot_announce_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_ANNOUNCE_H_ */
