/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_LINUX_PLATFORM_ADAPTERS_H_
#define MYBOT_LINUX_PLATFORM_ADAPTERS_H_

#include <mybot/mybot_build_config.h>
#include <mybot/platform/mybot_platform.h>

const mybot_audio_capture_ops_t *linux_audio_platform_alsa_capture_ops(void);
const mybot_audio_playback_ops_t *linux_audio_platform_alsa_playback_ops(void);
const mybot_audio_volume_ops_t *linux_audio_platform_alsa_volume_ops(void);
const mybot_kv_store_ops_t *linux_kv_store_platform_file_ops(void);
const mybot_key_ops_t *linux_key_platform_stdin_ops(void);
const mybot_lcd_ops_t *linux_lcd_platform_console_ops(void);
const mybot_announce_ops_t *linux_announce_platform_file_ops(void);
const mybot_wifi_ops_t *linux_wifi_platform_host_network_ops(void);
#if MYBOT_LINUX_HTTPS_OPENSSL
const mybot_https_ops_t *linux_https_platform_openssl_ops(void);
#endif

#endif /* MYBOT_LINUX_PLATFORM_ADAPTERS_H_ */
