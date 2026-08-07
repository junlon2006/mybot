#ifndef MYBOT_LINUX_BACKENDS_H_
#define MYBOT_LINUX_BACKENDS_H_

#include <mybot/mybot_build_config.h>

int linux_audio_platform_register_alsa_capture(void);
int linux_audio_platform_register_alsa_playback(void);
int linux_kv_store_platform_register_file(void);
int linux_key_platform_register_stdin(void);
int linux_lcd_platform_register_console(void);
int linux_wifi_platform_register_host_network(void);
#if MYBOT_LINUX_HTTPS_OPENSSL
int linux_https_platform_register_openssl(void);
#endif

#endif /* MYBOT_LINUX_BACKENDS_H_ */
