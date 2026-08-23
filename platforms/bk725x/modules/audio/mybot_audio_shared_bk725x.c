/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_audio_shared_bk725x.h"
#include "mybot_audio_playback_bk725x.h"
#include "mybot_audio_internal_bk725x.h"
#include "mybot_audio_power_bk725x.h"

#include "mybot_platform_log.h"
#include <os/os.h>

#include <stdbool.h>

#define TAG "mybot_pb"

/* ================================================================
 * Shared playback — single pipeline for all audio sources
 *
 * The prompt player and the SDK share a single playback pipeline
 * (raw_stream → speaker).  The prompt player writes PCM directly to
 * the pipeline; the SDK's playback worker writes through the platform
 * ops interface, which the adapter redirects to this same pipeline.
 * This eliminates the dual-pipeline design where the prompt player
 * created its own independent pipeline that conflicted with the SDK's.
 *
 * The SDK runtime owns the playback ring buffer used for RTC downlink;
 * this module only manages the platform playback pipeline.
 * ================================================================ */

static struct {
    void *pb_ctx;
    bool started;
    beken_mutex_t lock;
} s_shared;

static int shared_ensure_lock(void) {
    if (s_shared.lock) {
        return 0;
    }
    if (rtos_init_mutex(&s_shared.lock) != BK_OK) {
        MYBOT_LOGE(TAG, "shared playback lock init failed");
        return -1;
    }
    return 0;
}

int mybot_audio_bk725x_shared_playback_start(void) {
    MYBOT_LOGI(TAG, "shared playback start requested");

    if (shared_ensure_lock() < 0 || rtos_lock_mutex(&s_shared.lock) != BK_OK) {
        return -1;
    }

    if (s_shared.started) {
        (void)rtos_unlock_mutex(&s_shared.lock);
        return 0;
    }

    /* Acquire the audio performance vote once for the shared pipeline. */
    if (mybot_audio_bk725x_power_acquire() < 0) {
        MYBOT_LOGE(TAG, "shared playback power acquire failed");
        (void)rtos_unlock_mutex(&s_shared.lock);
        return -1;
    }

    int ret = mybot_audio_bk725x_playback_init(&s_shared.pb_ctx,
                                                MYBOT_PLAYBACK_RATE_HZ,
                                                MYBOT_PLAYBACK_CHANNELS,
                                                MYBOT_PLAYBACK_BITS);
    if (ret < 0) {
        MYBOT_LOGE(TAG, "shared pipeline init failed");
        (void)rtos_unlock_mutex(&s_shared.lock);
        return -1;
    }

    ret = mybot_audio_bk725x_playback_start(s_shared.pb_ctx);
    if (ret < 0) {
        MYBOT_LOGE(TAG, "shared pipeline start failed");
        mybot_audio_bk725x_playback_destroy(s_shared.pb_ctx);
        s_shared.pb_ctx = NULL;
        (void)rtos_unlock_mutex(&s_shared.lock);
        return -1;
    }

    /* Restore the user's volume on the freshly-created pipeline. */
    if (mybot_audio_bk725x_volume_apply() < 0) {
        MYBOT_LOGW(TAG, "shared playback volume apply failed, using default");
    }

    s_shared.started = true;
    MYBOT_LOGI(TAG, "shared playback ready");
    (void)rtos_unlock_mutex(&s_shared.lock);
    return 0;
}

int mybot_audio_bk725x_shared_playback_write(const int16_t *pcm, int frames) {
    if (shared_ensure_lock() < 0 || rtos_lock_mutex(&s_shared.lock) != BK_OK) {
        return -1;
    }
    if (!s_shared.started || !s_shared.pb_ctx) {
        (void)rtos_unlock_mutex(&s_shared.lock);
        return -1;
    }
    (void)rtos_unlock_mutex(&s_shared.lock);

    return mybot_audio_bk725x_playback_write(s_shared.pb_ctx, pcm, frames);
}

void *mybot_audio_bk725x_shared_playback_get_context(void) {
    return s_shared.pb_ctx;
}

bool mybot_audio_bk725x_shared_playback_is_started(void) {
    return s_shared.started;
}

void mybot_audio_bk725x_shared_playback_stop(void) {
    MYBOT_LOGI(TAG, "shared playback stop requested");

    if (shared_ensure_lock() < 0 || rtos_lock_mutex(&s_shared.lock) != BK_OK) {
        return;
    }
    if (!s_shared.started) {
        (void)rtos_unlock_mutex(&s_shared.lock);
        return;
    }

    if (s_shared.pb_ctx) {
        mybot_audio_bk725x_playback_stop(s_shared.pb_ctx);
        mybot_audio_bk725x_playback_destroy(s_shared.pb_ctx);
        s_shared.pb_ctx = NULL;
    }

    mybot_audio_bk725x_power_release();

    s_shared.started = false;
    MYBOT_LOGI(TAG, "shared playback stopped");
    (void)rtos_unlock_mutex(&s_shared.lock);
}
