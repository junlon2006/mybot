#ifndef MYBOT_LINUX_BACKENDS_H_
#define MYBOT_LINUX_BACKENDS_H_

int linux_audio_platform_register_alsa_capture(void);
int linux_audio_platform_register_alsa_playback(void);
int linux_kv_store_platform_register_file(void);
int linux_key_platform_register_stdin(void);
int linux_lcd_platform_register_console(void);
int linux_wifi_platform_register_host_network(void);

#endif /* MYBOT_LINUX_BACKENDS_H_ */
