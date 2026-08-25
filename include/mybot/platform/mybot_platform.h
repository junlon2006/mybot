/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_PLATFORM_H_
#define MYBOT_PLATFORM_H_

#include <mybot/mybot_export.h>
#include <mybot/platform/mybot_announce.h>
#include <mybot/platform/mybot_audio.h>
#include <mybot/platform/mybot_https.h>
#include <mybot/platform/mybot_key.h>
#include <mybot/platform/mybot_kv_store.h>
#include <mybot/platform/mybot_lcd.h>
#include <mybot/platform/mybot_wake_words.h>
#include <mybot/platform/mybot_wifi.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Process-wide platform implementation.
 *
 * Wi-Fi, KV store, key input, audio capture, and audio playback are required. The
 * remaining operations are optional, but the active SDK configuration may require
 * specific optional operations when mybot_start() is called.
 *
 * Every referenced operations table must provide all callbacks documented as required
 * by its operations type. Registration makes a shallow copy; all operations tables and
 * data referenced by those tables must remain valid for the process lifetime.
 */
typedef struct {
    /** Wi-Fi operations; required. */
    const mybot_wifi_ops_t *wifi;
    /** KV-store operations; required. */
    const mybot_kv_store_ops_t *kv_store;
    /** Key-input operations; required. */
    const mybot_key_ops_t *key;
    /** Capture operations; required. */
    const mybot_audio_capture_ops_t *audio_capture;
    /** Playback operations; required. */
    const mybot_audio_playback_ops_t *audio_playback;
    /** Optional hardware-volume operations. */
    const mybot_audio_volume_ops_t *audio_volume;
    /** Optional TLS transport operations. */
    const mybot_https_ops_t *https;
    /** Optional LCD operations. */
    const mybot_lcd_ops_t *lcd;
    /** Optional announcement operations. */
    const mybot_announce_ops_t *announce;
    /** Optional wake-word operations. */
    const mybot_wake_words_ops_t *wake_words;
} mybot_platform_descriptor_t;

/**
 * Validate and register one complete platform descriptor as a single commit.
 *
 * The call either commits the complete descriptor or leaves the registry unchanged.
 * One successful registration is allowed and must happen before mybot_start().
 *
 * @param descriptor complete descriptor satisfying mybot_platform_descriptor_t's operations and
 *                   lifetime contract
 * @return 0 on success; -1 if the descriptor is NULL or invalid, any registration
 *         already succeeded
 */
MYBOT_API int mybot_platform_register(const mybot_platform_descriptor_t *descriptor);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_PLATFORM_H_ */
