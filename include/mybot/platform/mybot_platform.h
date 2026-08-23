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

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYBOT_PLATFORM_API_VERSION 1U

#define MYBOT_PLATFORM_CAP_WIFI (UINT64_C(1) << 0)
#define MYBOT_PLATFORM_CAP_KV_STORE (UINT64_C(1) << 1)
#define MYBOT_PLATFORM_CAP_KEY (UINT64_C(1) << 2)
#define MYBOT_PLATFORM_CAP_AUDIO_CAPTURE (UINT64_C(1) << 3)
#define MYBOT_PLATFORM_CAP_AUDIO_PLAYBACK (UINT64_C(1) << 4)
#define MYBOT_PLATFORM_CAP_AUDIO_VOLUME (UINT64_C(1) << 5)
#define MYBOT_PLATFORM_CAP_HTTPS (UINT64_C(1) << 6)
#define MYBOT_PLATFORM_CAP_LCD (UINT64_C(1) << 7)
#define MYBOT_PLATFORM_CAP_ANNOUNCE (UINT64_C(1) << 8)
#define MYBOT_PLATFORM_CAP_WAKE_WORDS (UINT64_C(1) << 9)

#define MYBOT_PLATFORM_CAP_REQUIRED                                                                \
    (MYBOT_PLATFORM_CAP_WIFI | MYBOT_PLATFORM_CAP_KV_STORE | MYBOT_PLATFORM_CAP_KEY |              \
     MYBOT_PLATFORM_CAP_AUDIO_CAPTURE | MYBOT_PLATFORM_CAP_AUDIO_PLAYBACK)

typedef struct {
    uint32_t api_version;
    size_t struct_size;
    const char *name;
    uint64_t capabilities;

    const mybot_wifi_ops_t *wifi;
    const mybot_kv_store_ops_t *kv_store;
    const mybot_key_ops_t *key;
    const mybot_audio_capture_ops_t *audio_capture;
    const mybot_audio_playback_ops_t *audio_playback;
    const mybot_audio_volume_ops_t *audio_volume;
    const mybot_https_ops_t *https;
    const mybot_lcd_ops_t *lcd;
    const mybot_announce_ops_t *announce;
    const mybot_wake_words_ops_t *wake_words;
} mybot_platform_descriptor_t;

/**
 * Atomically validate and register one complete platform descriptor.
 *
 * The descriptor itself is copied. Its name and every referenced operations table must remain
 * valid for the process lifetime. Registration must happen exactly once, before mybot_start().
 * Descriptor registration and the legacy per-capability registration APIs cannot be mixed.
 */
MYBOT_API int mybot_platform_register(const mybot_platform_descriptor_t *descriptor);

/** Return the capabilities currently registered through either API style. */
MYBOT_API uint64_t mybot_platform_get_capabilities(void);

/** Validate required capabilities and optionally return the missing bit mask. */
MYBOT_API int mybot_platform_validate(uint64_t required_capabilities,
                                      uint64_t *missing_capabilities);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_PLATFORM_H_ */
