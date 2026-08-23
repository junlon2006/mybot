/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_LINUX_PLATFORM_ADAPTERS_H_
#define MYBOT_LINUX_PLATFORM_ADAPTERS_H_

#include <mybot/mybot_build_config.h>
#include <mybot/platform/mybot_platform.h>

const mybot_audio_capture_ops_t *linux_audio_platform_alsa_capture_ops(void);
int linux_audio_platform_register_alsa_capture(void);
const mybot_audio_playback_ops_t *linux_audio_platform_alsa_playback_ops(void);
int linux_audio_platform_register_alsa_playback(void);
const mybot_audio_volume_ops_t *linux_audio_platform_alsa_volume_ops(void);
int linux_audio_platform_register_alsa_volume(void);
const mybot_kv_store_ops_t *linux_kv_store_platform_file_ops(void);
int linux_kv_store_platform_register_file(void);
const mybot_key_ops_t *linux_key_platform_stdin_ops(void);
int linux_key_platform_register_stdin(void);
const mybot_lcd_ops_t *linux_lcd_platform_console_ops(void);
int linux_lcd_platform_register_console(void);
const mybot_announce_ops_t *linux_announce_platform_file_ops(void);
int linux_announce_platform_register(void);
const mybot_wifi_ops_t *linux_wifi_platform_host_network_ops(void);
int linux_wifi_platform_register_host_network(void);
#if MYBOT_LINUX_HTTPS_OPENSSL
const mybot_https_ops_t *linux_https_platform_openssl_ops(void);
int linux_https_platform_register_openssl(void);
#endif

#endif /* MYBOT_LINUX_PLATFORM_ADAPTERS_H_ */
