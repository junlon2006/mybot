/* SPDX-License-Identifier: Apache-2.0 */
#include "bk725x_platform_adapters_internal.h"
#include "mybot_audio_capture_bk725x.h"

#include <mybot/platform/mybot_audio.h>

#include "mybot_platform_log.h"

#include <stdbool.h>

#define TAG "mybot_cap"

static bool s_capture_registered;

static const mybot_audio_capture_ops_t s_bk725x_capture_ops = {
    .name = "bk725x-onboard-mic",
    .init = mybot_audio_bk725x_capture_init,
    .start = mybot_audio_bk725x_capture_start,
    .read = mybot_audio_bk725x_capture_read,
    .stop = mybot_audio_bk725x_capture_stop,
    .destroy = mybot_audio_bk725x_capture_destroy,
};

int bk725x_audio_platform_register_capture(void) {
    if (s_capture_registered) {
        return 0;
    }

    int result = mybot_audio_register_capture(&s_bk725x_capture_ops);
    if (result < 0) {
        MYBOT_LOGE(TAG, "registration failed");
        return result;
    }

    s_capture_registered = true;
    MYBOT_LOGI(TAG, "registered backend=%s", s_bk725x_capture_ops.name);
    return 0;
}
