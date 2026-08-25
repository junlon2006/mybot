/* SPDX-License-Identifier: Apache-2.0 */
#include "bk725x_platform_adapters.h"
#include "bk725x_platform_adapters_internal.h"

#include <mybot/platform/mybot_platform.h>

#include "mybot_platform_log.h"

#include <stdbool.h>

#define TAG "mybot_adapter"

static bool s_registered;

int bk725x_platform_adapters_register(void) {
    if (s_registered) {
        return 0;
    }

    mybot_platform_descriptor_t descriptor = {
        .wifi = bk725x_wifi_platform_ops_connectivity(),
        .kv_store = bk725x_kv_store_platform_ops_bk_env(),
        .key = bk725x_key_platform_ops_button(),
        .audio_capture = bk725x_audio_platform_ops_capture(),
        .audio_playback = bk725x_audio_platform_ops_playback(),
        .audio_volume = bk725x_audio_platform_ops_volume(),
        .https = bk725x_https_platform_ops_mbedtls(),
        .lcd = bk725x_lcd_platform_ops_dual_screen(),
        .announce = bk725x_announce_platform_ops_pcm(),
    };

    if (mybot_platform_register(&descriptor) < 0) {
        MYBOT_LOGE(TAG, "platform descriptor registration failed");
        return -1;
    }

    s_registered = true;
    MYBOT_LOGI(TAG, "platform adapters ready");
    return 0;
}
