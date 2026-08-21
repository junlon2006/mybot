/* SPDX-License-Identifier: Apache-2.0 */
#include "bk725x_platform_adapters.h"
#include "bk725x_platform_adapters_internal.h"

#include "mybot_platform_log.h"

#define TAG "mybot_adapter"

int bk725x_platform_adapters_register(void) {
    if (bk725x_https_platform_register_mbedtls() < 0) {
        MYBOT_LOGE(TAG, "HTTPS adapter registration failed");
        return -1;
    }
    if (bk725x_kv_store_platform_register_bk_env() < 0) {
        MYBOT_LOGE(TAG, "KV adapter registration failed");
        return -1;
    }
    if (bk725x_wifi_platform_register_connectivity() < 0) {
        MYBOT_LOGE(TAG, "Wi-Fi adapter registration failed");
        return -1;
    }
    if (bk725x_audio_platform_register_capture() < 0 ||
        bk725x_audio_platform_register_playback() < 0 ||
        bk725x_audio_platform_register_volume() < 0 ||
        bk725x_announce_platform_register_pcm() < 0) {
        MYBOT_LOGE(TAG, "audio adapter registration failed");
        return -1;
    }
    if (bk725x_key_platform_register_button() < 0) {
        MYBOT_LOGE(TAG, "key adapter registration failed");
        return -1;
    }
    if (bk725x_lcd_platform_register_dual_screen() < 0) {
        MYBOT_LOGE(TAG, "LCD adapter registration failed");
        return -1;
    }

    MYBOT_LOGI(TAG, "platform adapters ready");
    return 0;
}
