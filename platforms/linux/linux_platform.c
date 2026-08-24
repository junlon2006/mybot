/* SPDX-License-Identifier: Apache-2.0 */
#include "linux_platform.h"

#include "linux_platform_adapters.h"

#include <mybot/mybot_build_config.h>
#include <mybot/platform/mybot_platform.h>

int linux_platform_register(void) {
    mybot_platform_descriptor_t descriptor = {
        .wifi = linux_wifi_platform_host_network_ops(),
        .kv_store = linux_kv_store_platform_file_ops(),
        .key = linux_key_platform_stdin_ops(),
        .audio_capture = linux_audio_platform_alsa_capture_ops(),
        .audio_playback = linux_audio_platform_alsa_playback_ops(),
        .audio_volume = linux_audio_platform_alsa_volume_ops(),
        .lcd = linux_lcd_platform_console_ops(),
        .announce = linux_announce_platform_file_ops(),
#if MYBOT_LINUX_HTTPS_OPENSSL
        .https = linux_https_platform_openssl_ops(),
#endif
    };
    return mybot_platform_register(&descriptor);
}
