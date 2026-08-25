/* SPDX-License-Identifier: Apache-2.0 */
#include "bk725x_platform_adapters_internal.h"
#include "mybot_audio_playback_bk725x.h"
#include "mybot_audio_shared_bk725x.h"

#include <mybot/platform/mybot_audio.h>

#include "mybot_platform_log.h"

#define TAG "mybot_pb"

/* ---- Wrappers that redirect to the shared playback pipeline when active.
 * When the controller starts the shared pipeline early (for the provisioning
 * prompt), the SDK's ops->init / start / destroy must reuse the same pipeline
 * instead of creating a second one.  write and stop always delegate to the
 * underlying function whose context (shared or private) was set by init. ---- */

static int adapter_playback_init(void **ctx, int rate, int channels, int bits) {
    MYBOT_LOGI(TAG, "adapter init: rate=%d ch=%d bits=%d", rate, channels, bits);
    if (mybot_audio_bk725x_shared_playback_is_started()) {
        *ctx = mybot_audio_bk725x_shared_playback_get_context();
        if (!*ctx) {
            return -1;
        }
        MYBOT_LOGI(TAG, "shared playback: using existing pipeline");
        return 0;
    }
    return mybot_audio_bk725x_playback_init(ctx, rate, channels, bits);
}

static int adapter_playback_start(void *ctx) {
    MYBOT_LOGI(TAG, "adapter start");
    if (mybot_audio_bk725x_shared_playback_is_started()) {
        return 0; /* already running */
    }
    return mybot_audio_bk725x_playback_start(ctx);
}

static int adapter_playback_write(void *ctx, const void *buf, int frames) {
    int result = mybot_audio_bk725x_playback_write(ctx, buf, frames);
    if (result < 0) {
        MYBOT_LOGE(TAG, "adapter write failed: ctx=%p frames=%d", ctx, frames);
    }
    return result;
}

static int adapter_playback_stop(void *ctx) {
    MYBOT_LOGI(TAG, "adapter stop");
    if (mybot_audio_bk725x_shared_playback_is_started()) {
        return 0; /* shared module manages lifecycle */
    }
    return mybot_audio_bk725x_playback_stop(ctx);
}

static void adapter_playback_destroy(void *ctx) {
    MYBOT_LOGI(TAG, "adapter destroy");
    if (mybot_audio_bk725x_shared_playback_is_started()) {
        return; /* shared module manages lifecycle */
    }
    mybot_audio_bk725x_playback_destroy(ctx);
}

static const mybot_audio_playback_ops_t s_bk725x_playback_ops = {
    .init = adapter_playback_init,
    .start = adapter_playback_start,
    .write = adapter_playback_write,
    .stop = adapter_playback_stop,
    .destroy = adapter_playback_destroy,
};

const mybot_audio_playback_ops_t *bk725x_audio_platform_ops_playback(void) {
    return &s_bk725x_playback_ops;
}
