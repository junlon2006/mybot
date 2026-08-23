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

/** Descriptor ABI version accepted by mybot_platform_register(). */
#define MYBOT_PLATFORM_API_VERSION 1U

/** Platform provides Wi-Fi connectivity. */
#define MYBOT_PLATFORM_CAP_WIFI (UINT64_C(1) << 0)
/** Platform provides persistent key-value storage. */
#define MYBOT_PLATFORM_CAP_KV_STORE (UINT64_C(1) << 1)
/** Platform provides key input. */
#define MYBOT_PLATFORM_CAP_KEY (UINT64_C(1) << 2)
/** Platform provides PCM audio capture. */
#define MYBOT_PLATFORM_CAP_AUDIO_CAPTURE (UINT64_C(1) << 3)
/** Platform provides PCM audio playback. */
#define MYBOT_PLATFORM_CAP_AUDIO_PLAYBACK (UINT64_C(1) << 4)
/** Platform provides optional hardware volume control. */
#define MYBOT_PLATFORM_CAP_AUDIO_VOLUME (UINT64_C(1) << 5)
/** Platform provides an optional TLS transport for the built-in HTTPS client. */
#define MYBOT_PLATFORM_CAP_HTTPS (UINT64_C(1) << 6)
/** Platform provides an optional LCD renderer. */
#define MYBOT_PLATFORM_CAP_LCD (UINT64_C(1) << 7)
/** Platform provides optional local announcement audio. */
#define MYBOT_PLATFORM_CAP_ANNOUNCE (UINT64_C(1) << 8)
/** Platform provides optional local wake-word detection. */
#define MYBOT_PLATFORM_CAP_WAKE_WORDS (UINT64_C(1) << 9)

/**
 * Capabilities every descriptor must provide.
 *
 * Optional capabilities may still be required by the active SDK configuration;
 * mybot_start() validates those requirements before creating platform resources.
 */
#define MYBOT_PLATFORM_CAP_REQUIRED                                                                \
    (MYBOT_PLATFORM_CAP_WIFI | MYBOT_PLATFORM_CAP_KV_STORE | MYBOT_PLATFORM_CAP_KEY |              \
     MYBOT_PLATFORM_CAP_AUDIO_CAPTURE | MYBOT_PLATFORM_CAP_AUDIO_PLAYBACK)

/**
 * Versioned, self-describing platform implementation.
 *
 * A descriptor must advertise every supported implementation in @ref capabilities.
 * Each advertised capability must have a non-NULL matching operations pointer, and
 * each unadvertised capability must have a NULL pointer. The required capability set
 * in MYBOT_PLATFORM_CAP_REQUIRED must always be present. Unknown capability bits are
 * rejected.
 *
 * Every referenced operations table must have a non-empty name and all callbacks
 * documented as required by its operations type. Registration makes a shallow copy:
 * @ref name, all operations tables, and data referenced by those tables must remain
 * valid for the process lifetime.
 */
typedef struct {
    /** Must equal MYBOT_PLATFORM_API_VERSION. */
    uint32_t api_version;

    /** Size of this descriptor in bytes; set to sizeof(mybot_platform_descriptor_t). */
    size_t struct_size;

    /** Non-empty platform name used for diagnostics; retained after registration. */
    const char *name;

    /** Bitwise OR of MYBOT_PLATFORM_CAP_* values; unknown bits are invalid. */
    uint64_t capabilities;

    /** Wi-Fi operations; required and paired with MYBOT_PLATFORM_CAP_WIFI. */
    const mybot_wifi_ops_t *wifi;
    /** KV-store operations; required and paired with MYBOT_PLATFORM_CAP_KV_STORE. */
    const mybot_kv_store_ops_t *kv_store;
    /** Key-input operations; required and paired with MYBOT_PLATFORM_CAP_KEY. */
    const mybot_key_ops_t *key;
    /** Capture operations; required and paired with MYBOT_PLATFORM_CAP_AUDIO_CAPTURE. */
    const mybot_audio_capture_ops_t *audio_capture;
    /** Playback operations; required and paired with MYBOT_PLATFORM_CAP_AUDIO_PLAYBACK. */
    const mybot_audio_playback_ops_t *audio_playback;
    /** Optional hardware-volume operations, paired with MYBOT_PLATFORM_CAP_AUDIO_VOLUME. */
    const mybot_audio_volume_ops_t *audio_volume;
    /** Optional TLS transport operations, paired with MYBOT_PLATFORM_CAP_HTTPS. */
    const mybot_https_ops_t *https;
    /** Optional LCD operations, paired with MYBOT_PLATFORM_CAP_LCD. */
    const mybot_lcd_ops_t *lcd;
    /** Optional announcement operations, paired with MYBOT_PLATFORM_CAP_ANNOUNCE. */
    const mybot_announce_ops_t *announce;
    /** Optional wake-word operations, paired with MYBOT_PLATFORM_CAP_WAKE_WORDS. */
    const mybot_wake_words_ops_t *wake_words;
} mybot_platform_descriptor_t;

/**
 * Atomically validate and register one complete platform descriptor.
 *
 * The call either commits the complete descriptor or leaves the registry unchanged.
 * One successful registration is allowed and must happen before mybot_start(). The
 * descriptor path cannot be mixed with legacy per-capability registration: a successful
 * call to either style makes calls to the other style fail.
 *
 * @param descriptor complete descriptor satisfying mybot_platform_descriptor_t's
 *                   version, capability, operations, and lifetime contract
 * @return 0 on success; -1 if the descriptor is NULL or invalid, any registration
 *         already succeeded, or platform registration has been locked by startup
 */
MYBOT_API int mybot_platform_register(const mybot_platform_descriptor_t *descriptor);

/**
 * Return the capabilities currently registered through either API style.
 *
 * @return registered MYBOT_PLATFORM_CAP_* bit mask, or 0 before registration
 */
MYBOT_API uint64_t mybot_platform_get_capabilities(void);

/**
 * Check whether all requested capabilities are currently registered.
 *
 * This function does not register or lock anything. It may be used with either
 * descriptor or legacy registration and accepts any requested bit mask.
 *
 * @param required_capabilities capabilities required by the caller
 * @param missing_capabilities  [out] missing bits; may be NULL. When non-NULL,
 *                              it is written on both success and failure.
 * @return 0 when every requested bit is registered, -1 otherwise
 */
MYBOT_API int mybot_platform_validate(uint64_t required_capabilities,
                                      uint64_t *missing_capabilities);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_PLATFORM_H_ */
