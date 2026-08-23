/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_platform_registry.h"

#include <assert.h>
#include <string.h>

static int init_simple(void **ctx) {
    *ctx = ctx;
    return 0;
}

static void destroy_simple(void *ctx) {
    (void)ctx;
}

static int wifi_init(void **ctx, const char *device_id, mybot_wifi_event_handler_t emit,
                     void *user_data) {
    (void)device_id;
    (void)emit;
    (void)user_data;
    return init_simple(ctx);
}

static int key_init(void **ctx, mybot_key_event_handler_t emit, void *user_data) {
    (void)emit;
    (void)user_data;
    return init_simple(ctx);
}

static int lcd_render(void *ctx, const mybot_lcd_content_t *content) {
    (void)ctx;
    (void)content;
    return 0;
}

static int kv_get(void *ctx, const char *key, void *value, size_t capacity, size_t *out_len) {
    (void)ctx;
    (void)key;
    (void)value;
    (void)capacity;
    (void)out_len;
    return -1;
}

static int kv_set(void *ctx, const char *key, const void *value, size_t len) {
    (void)ctx;
    (void)key;
    (void)value;
    (void)len;
    return 0;
}

static int kv_erase(void *ctx, const char *key) {
    (void)ctx;
    (void)key;
    return 0;
}

static int audio_init(void **ctx, int rate, int channels, int bits) {
    (void)rate;
    (void)channels;
    (void)bits;
    return init_simple(ctx);
}

static int audio_start(void *ctx) {
    (void)ctx;
    return 0;
}

static int audio_read(void *ctx, void *buffer, int frames) {
    (void)ctx;
    (void)buffer;
    return frames;
}

static int audio_write(void *ctx, const void *buffer, int frames) {
    (void)ctx;
    (void)buffer;
    return frames;
}

static int audio_stop(void *ctx) {
    (void)ctx;
    return 0;
}

static const mybot_wifi_ops_t s_wifi = {
    .name = "wifi",
    .init = wifi_init,
    .destroy = destroy_simple,
};
static const mybot_kv_store_ops_t s_kv = {
    .name = "kv",
    .init = init_simple,
    .get = kv_get,
    .set = kv_set,
    .erase = kv_erase,
    .destroy = destroy_simple,
};
static const mybot_key_ops_t s_key = {
    .name = "key",
    .init = key_init,
    .destroy = destroy_simple,
};
static const mybot_lcd_ops_t s_lcd = {
    .name = "lcd",
    .init = init_simple,
    .render = lcd_render,
    .destroy = destroy_simple,
};
static const mybot_audio_capture_ops_t s_capture = {
    .name = "capture",
    .init = audio_init,
    .start = audio_start,
    .read = audio_read,
    .stop = audio_stop,
    .destroy = destroy_simple,
};
static const mybot_audio_playback_ops_t s_playback = {
    .name = "playback",
    .init = audio_init,
    .start = audio_start,
    .write = audio_write,
    .stop = audio_stop,
    .destroy = destroy_simple,
};

static mybot_platform_descriptor_t complete_descriptor(void) {
    mybot_platform_descriptor_t descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.api_version = MYBOT_PLATFORM_API_VERSION;
    descriptor.struct_size = sizeof(descriptor);
    descriptor.name = "test-platform";
    descriptor.capabilities = MYBOT_PLATFORM_CAP_REQUIRED;
    descriptor.wifi = &s_wifi;
    descriptor.kv_store = &s_kv;
    descriptor.key = &s_key;
    descriptor.audio_capture = &s_capture;
    descriptor.audio_playback = &s_playback;
    return descriptor;
}

int main(void) {
    mybot_platform_descriptor_t descriptor = complete_descriptor();

    descriptor.api_version++;
    assert(mybot_platform_register(&descriptor) < 0);
    assert(mybot_platform_get_capabilities() == 0);

    descriptor = complete_descriptor();
    descriptor.capabilities &= ~MYBOT_PLATFORM_CAP_KEY;
    descriptor.key = NULL;
    assert(mybot_platform_register(&descriptor) < 0);
    assert(mybot_platform_get_capabilities() == 0);

    descriptor = complete_descriptor();
    descriptor.lcd = &s_lcd;
    assert(mybot_platform_register(&descriptor) < 0);
    assert(mybot_platform_get_capabilities() == 0);

    mybot_wifi_ops_t unnamed_wifi = s_wifi;
    unnamed_wifi.name = NULL;
    descriptor = complete_descriptor();
    descriptor.wifi = &unnamed_wifi;
    assert(mybot_platform_register(&descriptor) < 0);
    assert(mybot_platform_get_capabilities() == 0);

    descriptor = complete_descriptor();
    assert(mybot_platform_register(&descriptor) == 0);
    assert(mybot_platform_get_capabilities() == MYBOT_PLATFORM_CAP_REQUIRED);
    assert(mybot_platform_registry_wifi() == &s_wifi);
    assert(mybot_platform_registry_kv_store() == &s_kv);
    assert(mybot_platform_registry_key() == &s_key);
    assert(mybot_platform_registry_audio_capture() == &s_capture);
    assert(mybot_platform_registry_audio_playback() == &s_playback);

    uint64_t missing = 0;
    assert(mybot_platform_validate(MYBOT_PLATFORM_CAP_REQUIRED, &missing) == 0);
    assert(missing == 0);
    assert(mybot_platform_validate(MYBOT_PLATFORM_CAP_REQUIRED | MYBOT_PLATFORM_CAP_HTTPS,
                                   &missing) < 0);
    assert(missing == MYBOT_PLATFORM_CAP_HTTPS);

    assert(mybot_platform_register(&descriptor) < 0);
    mybot_platform_registry_lock();
    assert(mybot_platform_registry_register_wifi(&s_wifi) < 0);
    return 0;
}
