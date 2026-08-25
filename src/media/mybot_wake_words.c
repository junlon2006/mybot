/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_wake_words.h>

#include "mybot_wake_words_internal.h"
#include "mybot_platform_registry.h"

#include <stddef.h>

int mybot_wake_words_init(mybot_wake_words_t *wake_words, int sample_rate, int channels,
                          int bits_per_sample, mybot_wake_words_handler_t handler,
                          void *user_data) {
    if (!wake_words || wake_words->active || !mybot_platform_registry_get()->wake_words ||
        sample_rate <= 0 || channels <= 0 || bits_per_sample <= 0 || !handler) {
        return -1;
    }

    wake_words->ops = mybot_platform_registry_get()->wake_words;
    if (wake_words->ops->init(&wake_words->ctx, sample_rate, channels, bits_per_sample, handler,
                              user_data) < 0) {
        wake_words->ctx = NULL;
        return -1;
    }

    wake_words->active = true;
    return 0;
}

int mybot_wake_words_process(mybot_wake_words_t *wake_words, const void *pcm, int frames) {
    if (!wake_words || !wake_words->active || !pcm || frames <= 0) {
        return -1;
    }
    return wake_words->ops->process(wake_words->ctx, pcm, frames);
}

void mybot_wake_words_deinit(mybot_wake_words_t *wake_words) {
    if (!wake_words || !wake_words->active) {
        return;
    }

    wake_words->ops->destroy(wake_words->ctx);
    wake_words->ctx = NULL;
    wake_words->active = false;
}
