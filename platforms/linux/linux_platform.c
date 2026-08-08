/* SPDX-License-Identifier: Apache-2.0 */
#include "linux_platform.h"

#include "linux_backends.h"

#include <mybot/mybot_build_config.h>

int linux_platform_register(void) {
    if (linux_wifi_platform_register_host_network() < 0 ||
        linux_audio_platform_register_alsa_capture() < 0 ||
        linux_audio_platform_register_alsa_playback() < 0 ||
        linux_audio_platform_register_alsa_volume() < 0 ||
        linux_kv_store_platform_register_file() < 0 || linux_key_platform_register_stdin() < 0 ||
        linux_lcd_platform_register_console() < 0
#if MYBOT_LINUX_HTTPS_OPENSSL
        || linux_https_platform_register_openssl() < 0
#endif
    ) {
        return -1;
    }
    return 0;
}
