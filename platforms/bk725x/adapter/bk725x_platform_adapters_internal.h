/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_BK725X_PLATFORM_ADAPTERS_INTERNAL_H_
#define MYBOT_BK725X_PLATFORM_ADAPTERS_INTERNAL_H_

#include <mybot/platform/mybot_announce.h>
#include <mybot/platform/mybot_audio.h>
#include <mybot/platform/mybot_https.h>
#include <mybot/platform/mybot_key.h>
#include <mybot/platform/mybot_kv_store.h>
#include <mybot/platform/mybot_lcd.h>
#include <mybot/platform/mybot_wifi.h>

const mybot_https_ops_t *bk725x_https_platform_ops_mbedtls(void);
const mybot_kv_store_ops_t *bk725x_kv_store_platform_ops_bk_env(void);
const mybot_wifi_ops_t *bk725x_wifi_platform_ops_connectivity(void);
const mybot_audio_capture_ops_t *bk725x_audio_platform_ops_capture(void);
const mybot_audio_playback_ops_t *bk725x_audio_platform_ops_playback(void);
const mybot_audio_volume_ops_t *bk725x_audio_platform_ops_volume(void);
const mybot_announce_ops_t *bk725x_announce_platform_ops_pcm(void);
const mybot_key_ops_t *bk725x_key_platform_ops_button(void);
const mybot_lcd_ops_t *bk725x_lcd_platform_ops_dual_screen(void);

#endif /* MYBOT_BK725X_PLATFORM_ADAPTERS_INTERNAL_H_ */
