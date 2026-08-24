/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_WAKE_WORDS_INTERNAL_H_
#define MYBOT_WAKE_WORDS_INTERNAL_H_

#include <mybot/platform/mybot_wake_words.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const mybot_wake_words_ops_t *ops;
    void *ctx;
    bool active;
} mybot_wake_words_t;

/**
 * SDK-internal wake-word facade. The public mybot/platform/mybot_wake_words.h
 * only exposes the platform contract (handler typedef, ops table and
 * mybot_platform_register()); the SDK audio pipeline drives the registered
 * implementation through the functions below.
 */

/** Initialize the registered implementation for the capture PCM format. */
int mybot_wake_words_init(mybot_wake_words_t *wake_words, int sample_rate, int channels,
                          int bits_per_sample, mybot_wake_words_handler_t handler, void *user_data);

/** Feed captured interleaved PCM frames to the local ASR implementation. */
int mybot_wake_words_process(mybot_wake_words_t *wake_words, const void *pcm, int frames);

/** Stop local ASR and release its resources. Idempotent. */
void mybot_wake_words_deinit(mybot_wake_words_t *wake_words);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_WAKE_WORDS_INTERNAL_H_ */
