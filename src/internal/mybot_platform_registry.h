/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_PLATFORM_REGISTRY_H_
#define MYBOT_PLATFORM_REGISTRY_H_

#include <mybot/platform/mybot_platform.h>

#include <stdbool.h>

int mybot_platform_registry_register_wifi(const mybot_wifi_ops_t *ops);
int mybot_platform_registry_register_kv_store(const mybot_kv_store_ops_t *ops);
int mybot_platform_registry_register_key(const mybot_key_ops_t *ops);
int mybot_platform_registry_register_audio_capture(const mybot_audio_capture_ops_t *ops);
int mybot_platform_registry_register_audio_playback(const mybot_audio_playback_ops_t *ops);
int mybot_platform_registry_register_audio_volume(const mybot_audio_volume_ops_t *ops);
int mybot_platform_registry_register_https(const mybot_https_ops_t *ops);
int mybot_platform_registry_register_lcd(const mybot_lcd_ops_t *ops);
int mybot_platform_registry_register_announce(const mybot_announce_ops_t *ops);
int mybot_platform_registry_register_wake_words(const mybot_wake_words_ops_t *ops);

const mybot_wifi_ops_t *mybot_platform_registry_wifi(void);
const mybot_kv_store_ops_t *mybot_platform_registry_kv_store(void);
const mybot_key_ops_t *mybot_platform_registry_key(void);
const mybot_audio_capture_ops_t *mybot_platform_registry_audio_capture(void);
const mybot_audio_playback_ops_t *mybot_platform_registry_audio_playback(void);
const mybot_audio_volume_ops_t *mybot_platform_registry_audio_volume(void);
const mybot_https_ops_t *mybot_platform_registry_https(void);
const mybot_lcd_ops_t *mybot_platform_registry_lcd(void);
const mybot_announce_ops_t *mybot_platform_registry_announce(void);
const mybot_wake_words_ops_t *mybot_platform_registry_wake_words(void);

void mybot_platform_registry_lock(void);

#endif /* MYBOT_PLATFORM_REGISTRY_H_ */
