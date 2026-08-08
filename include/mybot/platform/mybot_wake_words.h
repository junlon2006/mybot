/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_WAKE_WORDS_H_
#define MYBOT_WAKE_WORDS_H_

#include <stdbool.h>
#include <mybot/mybot_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/** wake_word is backend-owned and valid only for the duration of the callback. */
typedef void (*mybot_wake_words_handler_t)(const char *wake_word, void *user_data);

/**
 * Local ASR wake-word backend operations.
 *
 * process() receives interleaved PCM frames in the format passed to init(). The PCM pointer is
 * valid only for the duration of process(); an asynchronous backend must copy the data it needs.
 * A backend may emit detections from process() or its own worker thread. destroy() must stop the
 * backend and wait for all in-flight handlers to return.
 */
typedef struct {
    const char *name;
    int (*init)(void **ctx, int sample_rate, int channels, int bits_per_sample,
                mybot_wake_words_handler_t handler, void *user_data);
    int (*process)(void *ctx, const void *pcm, int frames);
    void (*destroy)(void *ctx);
} mybot_wake_words_ops_t;

/** Register the local ASR backend for the current platform. Call before mybot_app_start(). */
MYBOT_API int mybot_wake_words_register(const mybot_wake_words_ops_t *ops);

/** Return whether the current platform registered a local ASR backend. */
MYBOT_API bool mybot_wake_words_is_registered(void);

/** Initialize the registered backend for the capture PCM format. */
MYBOT_API int mybot_wake_words_init(int sample_rate, int channels, int bits_per_sample,
                                    mybot_wake_words_handler_t handler, void *user_data);

/** Feed captured interleaved PCM frames to the local ASR backend. */
MYBOT_API int mybot_wake_words_process(const void *pcm, int frames);

/** Stop local ASR and release its resources. Idempotent. */
MYBOT_API void mybot_wake_words_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_WAKE_WORDS_H_ */
