/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_BK725X_PLATFORM_ADAPTERS_INTERNAL_H_
#define MYBOT_BK725X_PLATFORM_ADAPTERS_INTERNAL_H_

int bk725x_https_platform_register_mbedtls(void);
int bk725x_kv_store_platform_register_bk_env(void);
int bk725x_wifi_platform_register_connectivity(void);
int bk725x_audio_platform_register_capture(void);
int bk725x_audio_platform_register_playback(void);
int bk725x_audio_platform_register_volume(void);
int bk725x_announce_platform_register_pcm(void);
int bk725x_key_platform_register_button(void);
int bk725x_lcd_platform_register_dual_screen(void);

#endif /* MYBOT_BK725X_PLATFORM_ADAPTERS_INTERNAL_H_ */
