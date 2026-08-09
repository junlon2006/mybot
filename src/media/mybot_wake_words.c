/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_wake_words.h>

#include "mybot_wake_words_internal.h"

#include <stddef.h>

static const mybot_wake_words_ops_t *s_ops;
static void *s_ctx;
static bool s_active;

int mybot_wake_words_register(const mybot_wake_words_ops_t *ops) {
    if (!ops || !ops->init || !ops->process || !ops->destroy || s_active) {
        return -1;
    }
    s_ops = ops;
    return 0;
}

bool mybot_wake_words_is_registered(void) {
    return s_ops != NULL;
}

int mybot_wake_words_init(int sample_rate, int channels, int bits_per_sample,
                          mybot_wake_words_handler_t handler, void *user_data) {
    if (s_active || !s_ops || sample_rate <= 0 || channels <= 0 || bits_per_sample <= 0 ||
        !handler) {
        return -1;
    }

    if (s_ops->init(&s_ctx, sample_rate, channels, bits_per_sample, handler, user_data) < 0) {
        s_ctx = NULL;
        return -1;
    }

    s_active = true;
    return 0;
}

int mybot_wake_words_process(const void *pcm, int frames) {
    if (!s_active || !pcm || frames <= 0) {
        return -1;
    }
    return s_ops->process(s_ctx, pcm, frames);
}

void mybot_wake_words_deinit(void) {
    if (!s_active) {
        return;
    }

    s_ops->destroy(s_ctx);
    s_ctx = NULL;
    s_active = false;
}
