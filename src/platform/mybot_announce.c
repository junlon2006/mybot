/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_announce_internal.h"

#include <api/aosl_log.h>
#include <hal/aosl_hal_thread.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Pairing codes are short (a few digits); 16 digits is generous. */
#define MYBOT_ANNOUNCE_MAX_CODE_LEN 16
#define MYBOT_ANNOUNCE_MAX_QUEUE (1 + MYBOT_ANNOUNCE_MAX_CODE_LEN)

static struct {
    const mybot_announce_ops_t *ops;
    void *ops_ctx;
    aosl_mutex_t lock;
    bool active; /* initialized */

    /* Queued announcement: prompt + one handle per code digit. */
    mybot_announce_sound_t queue[MYBOT_ANNOUNCE_MAX_QUEUE];
    void *handles[MYBOT_ANNOUNCE_MAX_QUEUE];
    int queue_len;
    int queue_pos;
} s_ann;

int mybot_announce_register(const mybot_announce_ops_t *ops) {
    if (!ops || !ops->init || !ops->open || !ops->read || !ops->close || !ops->destroy ||
        s_ann.ops) {
        return -1;
    }
    s_ann.ops = ops;
    return 0;
}

bool mybot_announce_is_registered(void) {
    return s_ann.ops != NULL;
}

int mybot_announce_init(void) {
    if (s_ann.active) {
        return 0;
    }
    if (!s_ann.ops) {
        return 0; /* optional feature, not registered */
    }

    s_ann.lock = aosl_hal_mutex_create();
    if (!s_ann.lock) {
        AOSL_LOG_ERR("announce: mutex create failed");
        return -1;
    }

    void *ctx = NULL;
    if (s_ann.ops->init(&ctx) < 0) {
        AOSL_LOG_ERR("announce: implementation init failed");
        aosl_hal_mutex_destroy(s_ann.lock);
        s_ann.lock = NULL;
        return -1;
    }

    s_ann.ops_ctx = ctx;
    s_ann.active = true;
    return 0;
}

void mybot_announce_deinit(void) {
    if (!s_ann.active) {
        return;
    }

    aosl_hal_mutex_lock(s_ann.lock);
    for (int i = 0; i < s_ann.queue_len; i++) {
        if (s_ann.handles[i]) {
            s_ann.ops->close(s_ann.ops_ctx, s_ann.handles[i]);
            s_ann.handles[i] = NULL;
        }
    }
    s_ann.queue_len = 0;
    s_ann.queue_pos = 0;
    aosl_hal_mutex_unlock(s_ann.lock);

    s_ann.ops->destroy(s_ann.ops_ctx);
    s_ann.ops_ctx = NULL;
    aosl_hal_mutex_destroy(s_ann.lock);
    s_ann.lock = NULL;
    s_ann.active = false;
}

int mybot_announce_play_pair_code(const char *code) {
    if (!s_ann.ops || !s_ann.active) {
        return -1;
    }
    if (!code) {
        code = "";
    }

    /* Build the sound list off the audio thread: opening assets may do file
     * or flash I/O. A missing prompt aborts the whole announcement; a missing
     * digit sound is skipped so the remaining digits still play. */
    mybot_announce_sound_t sounds[MYBOT_ANNOUNCE_MAX_QUEUE];
    void *handles[MYBOT_ANNOUNCE_MAX_QUEUE];
    int n = 0;

    sounds[n++] = MYBOT_ANNOUNCE_SOUND_PROMPT;
    for (const char *p = code; *p; p++) {
        if (*p < '0' || *p > '9') {
            AOSL_LOG_WRN("announce: ignoring non-digit '%c' in pair code", *p);
            continue;
        }
        if (n >= MYBOT_ANNOUNCE_MAX_QUEUE) {
            AOSL_LOG_WRN("announce: pair code longer than %d digits, "
                         "trailing digits not announced",
                         MYBOT_ANNOUNCE_MAX_CODE_LEN);
            break;
        }
        sounds[n++] = (mybot_announce_sound_t)(MYBOT_ANNOUNCE_SOUND_DIGIT_0 + (*p - '0'));
    }

    handles[0] = s_ann.ops->open(s_ann.ops_ctx, sounds[0]);
    if (!handles[0]) {
        AOSL_LOG_WRN("announce: prompt sound unavailable, announcement skipped");
        return -1;
    }
    int len = 1;
    for (int i = 1; i < n; i++) {
        void *h = s_ann.ops->open(s_ann.ops_ctx, sounds[i]);
        if (!h) {
            AOSL_LOG_WRN("announce: digit sound %d unavailable, skipped",
                         sounds[i] - MYBOT_ANNOUNCE_SOUND_DIGIT_0);
            continue;
        }
        sounds[len] = sounds[i];
        handles[len] = h;
        len++;
    }

    aosl_hal_mutex_lock(s_ann.lock);
    for (int i = 0; i < s_ann.queue_len; i++) {
        if (s_ann.handles[i]) {
            s_ann.ops->close(s_ann.ops_ctx, s_ann.handles[i]);
            s_ann.handles[i] = NULL;
        }
    }
    memcpy(s_ann.queue, sounds, (size_t)len * sizeof(sounds[0]));
    memcpy(s_ann.handles, handles, (size_t)len * sizeof(handles[0]));
    s_ann.queue_len = len;
    s_ann.queue_pos = 0;
    aosl_hal_mutex_unlock(s_ann.lock);

    AOSL_LOG_NTC("announce: pair code=%s queued, %d sound(s)", code, len);
    return 0;
}

void mybot_announce_stop(void) {
    if (!s_ann.active) {
        return;
    }

    aosl_hal_mutex_lock(s_ann.lock);
    for (int i = 0; i < s_ann.queue_len; i++) {
        if (s_ann.handles[i]) {
            s_ann.ops->close(s_ann.ops_ctx, s_ann.handles[i]);
            s_ann.handles[i] = NULL;
        }
    }
    s_ann.queue_len = 0;
    s_ann.queue_pos = 0;
    aosl_hal_mutex_unlock(s_ann.lock);
}

bool mybot_announce_is_active(void) {
    bool active = false;
    if (s_ann.active) {
        aosl_hal_mutex_lock(s_ann.lock);
        active = s_ann.queue_pos < s_ann.queue_len;
        aosl_hal_mutex_unlock(s_ann.lock);
    }
    return active;
}

int mybot_announce_read_pcm(int16_t *dst, int max_frames) {
    if (!dst || max_frames <= 0 || !s_ann.ops || !s_ann.active) {
        return 0;
    }

    aosl_hal_mutex_lock(s_ann.lock);
    int total = 0;
    while (total < max_frames && s_ann.queue_pos < s_ann.queue_len) {
        int n = s_ann.ops->read(s_ann.ops_ctx, s_ann.handles[s_ann.queue_pos], dst + total,
                                max_frames - total);
        if (n > 0) {
            total += n;
            break;
        }
        /* End (or error) of the current sound: advance to the next one. */
        s_ann.ops->close(s_ann.ops_ctx, s_ann.handles[s_ann.queue_pos]);
        s_ann.handles[s_ann.queue_pos] = NULL;
        s_ann.queue_pos++;
    }
    aosl_hal_mutex_unlock(s_ann.lock);
    return total;
}
