/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_platform_registry.h"

#include <string.h>

#define MYBOT_PLATFORM_CAP_ALL                                                                     \
    (MYBOT_PLATFORM_CAP_REQUIRED | MYBOT_PLATFORM_CAP_AUDIO_VOLUME | MYBOT_PLATFORM_CAP_HTTPS |    \
     MYBOT_PLATFORM_CAP_LCD | MYBOT_PLATFORM_CAP_ANNOUNCE | MYBOT_PLATFORM_CAP_WAKE_WORDS)

typedef enum {
    REGISTRY_EMPTY = 0,
    REGISTRY_LEGACY,
    REGISTRY_DESCRIPTOR,
} registry_mode_t;

static mybot_platform_descriptor_t s_registry;
static registry_mode_t s_mode;
static bool s_locked;

static bool name_is_valid(const char *name) {
    return name && name[0];
}

static bool wifi_is_valid(const mybot_wifi_ops_t *ops) {
    return ops && ops->init && ops->destroy;
}

static bool kv_store_is_valid(const mybot_kv_store_ops_t *ops) {
    return ops && ops->init && ops->get && ops->set && ops->erase && ops->destroy;
}

static bool key_is_valid(const mybot_key_ops_t *ops) {
    return ops && ops->init && ops->destroy;
}

static bool audio_capture_is_valid(const mybot_audio_capture_ops_t *ops) {
    return ops && ops->init && ops->start && ops->read && ops->stop && ops->destroy;
}

static bool audio_playback_is_valid(const mybot_audio_playback_ops_t *ops) {
    return ops && ops->init && ops->start && ops->write && ops->stop && ops->destroy;
}

static bool audio_volume_is_valid(const mybot_audio_volume_ops_t *ops) {
    return ops && ops->init && ops->set_volume && ops->destroy;
}

static bool https_is_valid(const mybot_https_ops_t *ops) {
    return ops && name_is_valid(ops->name) && ops->connect && ops->send && ops->recv && ops->close;
}

static bool lcd_is_valid(const mybot_lcd_ops_t *ops) {
    return ops && ops->init && ops->render && ops->destroy;
}

static bool announce_is_valid(const mybot_announce_ops_t *ops) {
    return ops && ops->init && ops->open && ops->read && ops->close && ops->destroy;
}

static bool wake_words_is_valid(const mybot_wake_words_ops_t *ops) {
    return ops && ops->init && ops->process && ops->destroy;
}

static bool pointer_matches(uint64_t capabilities, uint64_t bit, const void *ops) {
    return ((capabilities & bit) != 0) == (ops != NULL);
}

static bool ops_names_are_valid(const mybot_platform_descriptor_t *descriptor) {
    return name_is_valid(descriptor->wifi->name) && name_is_valid(descriptor->kv_store->name) &&
           name_is_valid(descriptor->key->name) && name_is_valid(descriptor->audio_capture->name) &&
           name_is_valid(descriptor->audio_playback->name) &&
           (!descriptor->audio_volume || name_is_valid(descriptor->audio_volume->name)) &&
           (!descriptor->https || name_is_valid(descriptor->https->name)) &&
           (!descriptor->lcd || name_is_valid(descriptor->lcd->name)) &&
           (!descriptor->announce || name_is_valid(descriptor->announce->name)) &&
           (!descriptor->wake_words || name_is_valid(descriptor->wake_words->name));
}

static bool descriptor_is_valid(const mybot_platform_descriptor_t *descriptor) {
    if (!descriptor || descriptor->api_version != MYBOT_PLATFORM_API_VERSION ||
        descriptor->struct_size < sizeof(*descriptor) || !name_is_valid(descriptor->name) ||
        (descriptor->capabilities & ~MYBOT_PLATFORM_CAP_ALL) != 0 ||
        (descriptor->capabilities & MYBOT_PLATFORM_CAP_REQUIRED) != MYBOT_PLATFORM_CAP_REQUIRED) {
        return false;
    }

    uint64_t caps = descriptor->capabilities;
    return pointer_matches(caps, MYBOT_PLATFORM_CAP_WIFI, descriptor->wifi) &&
           pointer_matches(caps, MYBOT_PLATFORM_CAP_KV_STORE, descriptor->kv_store) &&
           pointer_matches(caps, MYBOT_PLATFORM_CAP_KEY, descriptor->key) &&
           pointer_matches(caps, MYBOT_PLATFORM_CAP_AUDIO_CAPTURE, descriptor->audio_capture) &&
           pointer_matches(caps, MYBOT_PLATFORM_CAP_AUDIO_PLAYBACK, descriptor->audio_playback) &&
           pointer_matches(caps, MYBOT_PLATFORM_CAP_AUDIO_VOLUME, descriptor->audio_volume) &&
           pointer_matches(caps, MYBOT_PLATFORM_CAP_HTTPS, descriptor->https) &&
           pointer_matches(caps, MYBOT_PLATFORM_CAP_LCD, descriptor->lcd) &&
           pointer_matches(caps, MYBOT_PLATFORM_CAP_ANNOUNCE, descriptor->announce) &&
           pointer_matches(caps, MYBOT_PLATFORM_CAP_WAKE_WORDS, descriptor->wake_words) &&
           ops_names_are_valid(descriptor) && wifi_is_valid(descriptor->wifi) &&
           kv_store_is_valid(descriptor->kv_store) && key_is_valid(descriptor->key) &&
           audio_capture_is_valid(descriptor->audio_capture) &&
           audio_playback_is_valid(descriptor->audio_playback) &&
           (!descriptor->audio_volume || audio_volume_is_valid(descriptor->audio_volume)) &&
           (!descriptor->https || https_is_valid(descriptor->https)) &&
           (!descriptor->lcd || lcd_is_valid(descriptor->lcd)) &&
           (!descriptor->announce || announce_is_valid(descriptor->announce)) &&
           (!descriptor->wake_words || wake_words_is_valid(descriptor->wake_words));
}

int mybot_platform_register(const mybot_platform_descriptor_t *descriptor) {
    if (s_locked || s_mode != REGISTRY_EMPTY || !descriptor_is_valid(descriptor)) {
        return -1;
    }
    s_registry = *descriptor;
    s_mode = REGISTRY_DESCRIPTOR;
    return 0;
}

uint64_t mybot_platform_get_capabilities(void) {
    return s_registry.capabilities;
}

int mybot_platform_validate(uint64_t required_capabilities, uint64_t *missing_capabilities) {
    uint64_t missing = required_capabilities & ~s_registry.capabilities;
    if (missing_capabilities) {
        *missing_capabilities = missing;
    }
    return missing == 0 ? 0 : -1;
}

static bool legacy_slot_available(uint64_t capability) {
    if (s_locked || s_mode == REGISTRY_DESCRIPTOR || (s_registry.capabilities & capability) != 0) {
        return false;
    }
    s_mode = REGISTRY_LEGACY;
    s_registry.api_version = MYBOT_PLATFORM_API_VERSION;
    s_registry.struct_size = sizeof(s_registry);
    s_registry.name = "legacy-registration";
    return true;
}

#define DEFINE_LEGACY_REGISTER(function_name, type, field, capability, validator)                  \
    int function_name(const type *ops) {                                                           \
        if (!validator(ops) || !legacy_slot_available(capability)) {                               \
            return -1;                                                                             \
        }                                                                                          \
        s_registry.field = ops;                                                                    \
        s_registry.capabilities |= capability;                                                     \
        return 0;                                                                                  \
    }

DEFINE_LEGACY_REGISTER(mybot_platform_registry_register_wifi, mybot_wifi_ops_t, wifi,
                       MYBOT_PLATFORM_CAP_WIFI, wifi_is_valid)
DEFINE_LEGACY_REGISTER(mybot_platform_registry_register_kv_store, mybot_kv_store_ops_t, kv_store,
                       MYBOT_PLATFORM_CAP_KV_STORE, kv_store_is_valid)
DEFINE_LEGACY_REGISTER(mybot_platform_registry_register_key, mybot_key_ops_t, key,
                       MYBOT_PLATFORM_CAP_KEY, key_is_valid)
DEFINE_LEGACY_REGISTER(mybot_platform_registry_register_audio_capture, mybot_audio_capture_ops_t,
                       audio_capture, MYBOT_PLATFORM_CAP_AUDIO_CAPTURE, audio_capture_is_valid)
DEFINE_LEGACY_REGISTER(mybot_platform_registry_register_audio_playback, mybot_audio_playback_ops_t,
                       audio_playback, MYBOT_PLATFORM_CAP_AUDIO_PLAYBACK, audio_playback_is_valid)
DEFINE_LEGACY_REGISTER(mybot_platform_registry_register_audio_volume, mybot_audio_volume_ops_t,
                       audio_volume, MYBOT_PLATFORM_CAP_AUDIO_VOLUME, audio_volume_is_valid)
DEFINE_LEGACY_REGISTER(mybot_platform_registry_register_https, mybot_https_ops_t, https,
                       MYBOT_PLATFORM_CAP_HTTPS, https_is_valid)
DEFINE_LEGACY_REGISTER(mybot_platform_registry_register_lcd, mybot_lcd_ops_t, lcd,
                       MYBOT_PLATFORM_CAP_LCD, lcd_is_valid)
DEFINE_LEGACY_REGISTER(mybot_platform_registry_register_announce, mybot_announce_ops_t, announce,
                       MYBOT_PLATFORM_CAP_ANNOUNCE, announce_is_valid)
DEFINE_LEGACY_REGISTER(mybot_platform_registry_register_wake_words, mybot_wake_words_ops_t,
                       wake_words, MYBOT_PLATFORM_CAP_WAKE_WORDS, wake_words_is_valid)

#define DEFINE_GETTER(function_name, type, field)                                                  \
    const type *function_name(void) {                                                              \
        return s_registry.field;                                                                   \
    }

DEFINE_GETTER(mybot_platform_registry_wifi, mybot_wifi_ops_t, wifi)
DEFINE_GETTER(mybot_platform_registry_kv_store, mybot_kv_store_ops_t, kv_store)
DEFINE_GETTER(mybot_platform_registry_key, mybot_key_ops_t, key)
DEFINE_GETTER(mybot_platform_registry_audio_capture, mybot_audio_capture_ops_t, audio_capture)
DEFINE_GETTER(mybot_platform_registry_audio_playback, mybot_audio_playback_ops_t, audio_playback)
DEFINE_GETTER(mybot_platform_registry_audio_volume, mybot_audio_volume_ops_t, audio_volume)
DEFINE_GETTER(mybot_platform_registry_https, mybot_https_ops_t, https)
DEFINE_GETTER(mybot_platform_registry_lcd, mybot_lcd_ops_t, lcd)
DEFINE_GETTER(mybot_platform_registry_announce, mybot_announce_ops_t, announce)
DEFINE_GETTER(mybot_platform_registry_wake_words, mybot_wake_words_ops_t, wake_words)

void mybot_platform_registry_lock(void) {
    s_locked = true;
}
