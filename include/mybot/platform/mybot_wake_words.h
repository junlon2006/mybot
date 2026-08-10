/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_WAKE_WORDS_H_
#define MYBOT_WAKE_WORDS_H_

#include <stdbool.h>
#include <mybot/mybot_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Local ASR detection callback.
 *
 * @param wake_word detected wake word; owned by the implementation and valid only
 *                  for the duration of the callback
 * @param user_data opaque pointer supplied to the implementation at init() time
 *
 * @note Called from implementation or SDK audio context; keep it short.
 */
typedef void (*mybot_wake_words_handler_t)(const char *wake_word, void *user_data);

/**
 * Local ASR wake-word implementation operations.
 *
 * The implementation receives captured PCM frames and reports local detections.
 */
typedef struct {
    /** Implementation name for logging and diagnostics. */
    const char *name;

    /**
     * Allocate and start the local ASR engine.
     *
     * @param ctx             [out] implementation context handle
     * @param sample_rate     PCM sample rate in Hz (e.g. 16000)
     * @param channels        number of PCM channels (e.g. 1)
     * @param bits_per_sample bits per PCM sample (e.g. 16)
     * @param handler         detection callback
     * @param user_data       opaque pointer forwarded to handler()
     * @return 0 on success, -1 on error
     */
    int (*init)(void **ctx, int sample_rate, int channels, int bits_per_sample,
                mybot_wake_words_handler_t handler, void *user_data);

    /**
     * Feed one block of captured interleaved PCM frames.
     *
     * @param ctx    implementation context from init()
     * @param pcm    interleaved PCM frames in the format passed to init();
     *               borrowed for the duration of the call — an asynchronous
     *               implementation must copy the data it needs
     * @param frames number of frames in pcm
     * @return 0 on success, -1 on error
     *
     * @note An implementation may emit detections from process() or from its own
     *       worker thread.
     */
    int (*process)(void *ctx, const void *pcm, int frames);

    /**
     * Stop local ASR and release all resources.
     *
     * Must stop the implementation and wait for all in-flight detection handlers to
     * return before returning.
     *
     * @param ctx implementation context from init()
     */
    void (*destroy)(void *ctx);
} mybot_wake_words_ops_t;

/**
 * Register the local ASR implementation for the current platform.
 *
 * @param ops wake-word operations table; must remain valid for the process
 *            lifetime
 * @return 0 on success, -1 if ops is invalid or already registered
 *
 * @note Call exactly once, before mybot_start(). Required only when
 *       MYBOT_WAKE_WORDS=ON.
 */
MYBOT_API int mybot_wake_words_register(const mybot_wake_words_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_WAKE_WORDS_H_ */
