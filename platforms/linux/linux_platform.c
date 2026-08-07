#include "linux_platform.h"

#include "linux_backends.h"

int linux_platform_register(void) {
    if (linux_wifi_platform_register_host_network() < 0 ||
        linux_audio_platform_register_alsa_capture() < 0 ||
        linux_audio_platform_register_alsa_playback() < 0 ||
        linux_kv_store_platform_register_file() < 0 || linux_key_platform_register_stdin() < 0 ||
        linux_lcd_platform_register_console() < 0) {
        return -1;
    }
    return 0;
}
