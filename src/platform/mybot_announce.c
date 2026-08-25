/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_announce_internal.h"
#include "mybot_platform_registry.h"

#include <api/aosl_log.h>
#include <hal/aosl_hal_thread.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int mybot_announce_init(mybot_announce_t *announce) {
    if (!announce) {
        return -1;
    }
    if (announce && announce->active) {
        return 0;
    }
    announce->ops = mybot_platform_registry_get()->announce;
    if (!announce->ops) {
        return 0; /* optional feature, not registered */
    }

    announce->lock = aosl_hal_mutex_create();
    if (!announce->lock) {
        AOSL_LOG_ERR("announce: mutex create failed");
        return -1;
    }

    void *ctx = NULL;
    if (announce->ops->init(&ctx) < 0) {
        AOSL_LOG_ERR("announce: implementation init failed");
        aosl_hal_mutex_destroy(announce->lock);
        announce->lock = NULL;
        return -1;
    }

    announce->ops_ctx = ctx;
    announce->active = true;
    return 0;
}

void mybot_announce_deinit(mybot_announce_t *announce) {
    if (!announce || !announce->active) {
        return;
    }

    aosl_hal_mutex_lock(announce->lock);
    for (int i = 0; i < announce->queue_len; i++) {
        if (announce->handles[i]) {
            announce->ops->close(announce->ops_ctx, announce->handles[i]);
            announce->handles[i] = NULL;
        }
    }
    announce->queue_len = 0;
    announce->queue_pos = 0;
    aosl_hal_mutex_unlock(announce->lock);

    announce->ops->destroy(announce->ops_ctx);
    announce->ops_ctx = NULL;
    aosl_hal_mutex_destroy(announce->lock);
    announce->lock = NULL;
    announce->active = false;
}

int mybot_announce_play_pair_code(mybot_announce_t *announce, const char *code) {
    if (!announce || !announce->ops || !announce->active) {
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

    handles[0] = announce->ops->open(announce->ops_ctx, sounds[0]);
    if (!handles[0]) {
        AOSL_LOG_WRN("announce: prompt sound unavailable, announcement skipped");
        return -1;
    }
    int len = 1;
    for (int i = 1; i < n; i++) {
        void *h = announce->ops->open(announce->ops_ctx, sounds[i]);
        if (!h) {
            AOSL_LOG_WRN("announce: digit sound %d unavailable, skipped",
                         sounds[i] - MYBOT_ANNOUNCE_SOUND_DIGIT_0);
            continue;
        }
        sounds[len] = sounds[i];
        handles[len] = h;
        len++;
    }

    aosl_hal_mutex_lock(announce->lock);
    for (int i = 0; i < announce->queue_len; i++) {
        if (announce->handles[i]) {
            announce->ops->close(announce->ops_ctx, announce->handles[i]);
            announce->handles[i] = NULL;
        }
    }
    memcpy(announce->queue, sounds, (size_t)len * sizeof(sounds[0]));
    memcpy(announce->handles, handles, (size_t)len * sizeof(handles[0]));
    announce->queue_len = len;
    announce->queue_pos = 0;
    aosl_hal_mutex_unlock(announce->lock);

    AOSL_LOG_NTC("announce: pair code=%s queued, %d sound(s)", code, len);
    return 0;
}

void mybot_announce_stop(mybot_announce_t *announce) {
    if (!announce || !announce->active) {
        return;
    }

    aosl_hal_mutex_lock(announce->lock);
    for (int i = 0; i < announce->queue_len; i++) {
        if (announce->handles[i]) {
            announce->ops->close(announce->ops_ctx, announce->handles[i]);
            announce->handles[i] = NULL;
        }
    }
    announce->queue_len = 0;
    announce->queue_pos = 0;
    aosl_hal_mutex_unlock(announce->lock);
}

bool mybot_announce_is_active(mybot_announce_t *announce) {
    bool active = false;
    if (announce && announce->active) {
        aosl_hal_mutex_lock(announce->lock);
        active = announce->queue_pos < announce->queue_len;
        aosl_hal_mutex_unlock(announce->lock);
    }
    return active;
}

int mybot_announce_read_pcm(mybot_announce_t *announce, int16_t *dst, int max_frames) {
    if (!announce || !dst || max_frames <= 0 || !announce->ops || !announce->active) {
        return 0;
    }

    aosl_hal_mutex_lock(announce->lock);
    int total = 0;
    while (total < max_frames && announce->queue_pos < announce->queue_len) {
        int n = announce->ops->read(announce->ops_ctx, announce->handles[announce->queue_pos],
                                    dst + total, max_frames - total);
        if (n > 0) {
            total += n;
            break;
        }
        /* End (or error) of the current sound: advance to the next one. */
        announce->ops->close(announce->ops_ctx, announce->handles[announce->queue_pos]);
        announce->handles[announce->queue_pos] = NULL;
        announce->queue_pos++;
    }
    aosl_hal_mutex_unlock(announce->lock);
    return total;
}
