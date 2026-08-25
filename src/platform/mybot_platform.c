/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_platform_registry.h"

static mybot_platform_descriptor_t s_registry;
static bool s_registered;

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
    return ops && ops->connect && ops->send && ops->recv && ops->close;
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

static bool descriptor_is_valid(const mybot_platform_descriptor_t *descriptor) {
    return descriptor && wifi_is_valid(descriptor->wifi) &&
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
    if (s_registered || !descriptor_is_valid(descriptor)) {
        return -1;
    }
    s_registry = *descriptor;
    s_registered = true;
    return 0;
}

bool mybot_platform_registry_is_registered(void) {
    return s_registered;
}

const mybot_platform_descriptor_t *mybot_platform_registry_get(void) {
    return &s_registry;
}
