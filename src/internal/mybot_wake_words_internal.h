/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_WAKE_WORDS_INTERNAL_H_
#define MYBOT_WAKE_WORDS_INTERNAL_H_

#include <mybot/platform/mybot_wake_words.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * SDK-internal wake-word facade. The public mybot/platform/mybot_wake_words.h
 * only exposes the backend contract (handler typedef, ops table and
 * mybot_wake_words_register()); the SDK audio pipeline drives the registered
 * backend through the functions below.
 */

/** Return whether the current platform registered a local ASR backend. */
bool mybot_wake_words_is_registered(void);

/** Initialize the registered backend for the capture PCM format. */
int mybot_wake_words_init(int sample_rate, int channels, int bits_per_sample,
                          mybot_wake_words_handler_t handler, void *user_data);

/** Feed captured interleaved PCM frames to the local ASR backend. */
int mybot_wake_words_process(const void *pcm, int frames);

/** Stop local ASR and release its resources. Idempotent. */
void mybot_wake_words_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_WAKE_WORDS_INTERNAL_H_ */
