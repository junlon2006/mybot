/* SPDX-License-Identifier: Apache-2.0 */
#include "linux_platform.h"
#include "linux_platform_adapters.h"
#include "mybot_platform_registry.h"

#include <mybot/mybot_build_config.h>

#include <assert.h>

int main(void) {
    assert(linux_platform_register() == 0);
    assert(mybot_platform_registry_is_registered());
    const mybot_platform_descriptor_t *platform = mybot_platform_registry_get();
    assert(platform->wifi == linux_wifi_platform_host_network_ops());
    assert(platform->kv_store == linux_kv_store_platform_file_ops());
    assert(platform->key == linux_key_platform_stdin_ops());
    assert(platform->audio_capture == linux_audio_platform_alsa_capture_ops());
    assert(platform->audio_playback == linux_audio_platform_alsa_playback_ops());
    assert(platform->audio_volume == linux_audio_platform_alsa_volume_ops());
    assert(platform->lcd == linux_lcd_platform_console_ops());
    assert(platform->announce == linux_announce_platform_file_ops());
    assert(platform->wake_words == NULL);
#if MYBOT_LINUX_HTTPS_OPENSSL
    assert(platform->https == linux_https_platform_openssl_ops());
#else
    assert(platform->https == NULL);
#endif
    assert(linux_platform_register() < 0);
    return 0;
}
