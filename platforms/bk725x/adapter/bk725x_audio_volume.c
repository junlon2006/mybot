/* SPDX-License-Identifier: Apache-2.0 */
#include "bk725x_platform_adapters_internal.h"
#include "mybot_audio_volume_bk725x.h"

#include <mybot/platform/mybot_audio.h>

#include "mybot_platform_log.h"

#include <stdbool.h>

#define TAG "mybot_vol"

static bool s_volume_registered;

static const mybot_audio_volume_ops_t s_bk725x_volume_ops = {
    .name = "bk725x-onboard-speaker-volume",
    .init = mybot_audio_bk725x_volume_init,
    .set_volume = mybot_audio_bk725x_volume_set,
    .get_volume = mybot_audio_bk725x_volume_get,
    .destroy = mybot_audio_bk725x_volume_destroy,
};

int bk725x_audio_platform_register_volume(void) {
    if (s_volume_registered) {
        return 0;
    }
    int result = mybot_audio_device_register_volume(&s_bk725x_volume_ops);
    if (result < 0) {
        MYBOT_LOGE(TAG, "registration failed");
        return result;
    }

    s_volume_registered = true;
    MYBOT_LOGI(TAG, "registered backend=%s", s_bk725x_volume_ops.name);
    return 0;
}
