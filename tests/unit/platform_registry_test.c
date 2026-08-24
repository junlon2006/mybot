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

static int volume_set(void *ctx, int volume) {
    (void)ctx;
    (void)volume;
    return 0;
}

static int https_connect(void **connection, const char *host, uint16_t port, int timeout_ms) {
    (void)host;
    (void)port;
    (void)timeout_ms;
    return init_simple(connection);
}

static int https_send(void *connection, const void *data, size_t len, int timeout_ms) {
    (void)connection;
    (void)data;
    (void)timeout_ms;
    return (int)len;
}

static int https_recv(void *connection, void *data, size_t capacity, int timeout_ms) {
    (void)connection;
    (void)data;
    (void)capacity;
    (void)timeout_ms;
    return 0;
}

static void *announce_open(void *ctx, mybot_announce_sound_t sound) {
    (void)sound;
    return ctx;
}

static int announce_read(void *ctx, void *sound, int16_t *dst, int max_frames) {
    (void)ctx;
    (void)sound;
    (void)dst;
    (void)max_frames;
    return 0;
}

static void announce_close(void *ctx, void *sound) {
    (void)ctx;
    (void)sound;
}

static int wake_words_init(void **ctx, int sample_rate, int channels, int bits_per_sample,
                           mybot_wake_words_handler_t handler, void *user_data) {
    (void)sample_rate;
    (void)channels;
    (void)bits_per_sample;
    (void)handler;
    (void)user_data;
    return init_simple(ctx);
}

static int wake_words_process(void *ctx, const void *pcm, int frames) {
    (void)ctx;
    (void)pcm;
    (void)frames;
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
static const mybot_audio_volume_ops_t s_volume = {
    .name = "volume",
    .init = init_simple,
    .set_volume = volume_set,
    .destroy = destroy_simple,
};
static const mybot_https_ops_t s_https = {
    .name = "https",
    .connect = https_connect,
    .send = https_send,
    .recv = https_recv,
    .close = destroy_simple,
};
static const mybot_announce_ops_t s_announce = {
    .name = "announce",
    .init = init_simple,
    .open = announce_open,
    .read = announce_read,
    .close = announce_close,
    .destroy = destroy_simple,
};
static const mybot_wake_words_ops_t s_wake_words = {
    .name = "wake-words",
    .init = wake_words_init,
    .process = wake_words_process,
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

    assert(mybot_platform_register(NULL) < 0);

    descriptor.struct_size--;
    assert(mybot_platform_register(&descriptor) < 0);

    descriptor = complete_descriptor();
    descriptor.name = NULL;
    assert(mybot_platform_register(&descriptor) < 0);

    descriptor = complete_descriptor();
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

    mybot_wifi_ops_t invalid_wifi = s_wifi;
    invalid_wifi.init = NULL;
    descriptor = complete_descriptor();
    descriptor.wifi = &invalid_wifi;
    assert(mybot_platform_register(&descriptor) < 0);

    mybot_kv_store_ops_t invalid_kv = s_kv;
    invalid_kv.get = NULL;
    descriptor = complete_descriptor();
    descriptor.kv_store = &invalid_kv;
    assert(mybot_platform_register(&descriptor) < 0);

    mybot_key_ops_t invalid_key = s_key;
    invalid_key.init = NULL;
    descriptor = complete_descriptor();
    descriptor.key = &invalid_key;
    assert(mybot_platform_register(&descriptor) < 0);

    mybot_audio_capture_ops_t invalid_capture = s_capture;
    invalid_capture.read = NULL;
    descriptor = complete_descriptor();
    descriptor.audio_capture = &invalid_capture;
    assert(mybot_platform_register(&descriptor) < 0);

    mybot_audio_playback_ops_t invalid_playback = s_playback;
    invalid_playback.write = NULL;
    descriptor = complete_descriptor();
    descriptor.audio_playback = &invalid_playback;
    assert(mybot_platform_register(&descriptor) < 0);

    mybot_audio_volume_ops_t invalid_volume = s_volume;
    invalid_volume.set_volume = NULL;
    descriptor = complete_descriptor();
    descriptor.capabilities |= MYBOT_PLATFORM_CAP_AUDIO_VOLUME;
    descriptor.audio_volume = &invalid_volume;
    assert(mybot_platform_register(&descriptor) < 0);

    mybot_https_ops_t invalid_https = s_https;
    invalid_https.recv = NULL;
    descriptor = complete_descriptor();
    descriptor.capabilities |= MYBOT_PLATFORM_CAP_HTTPS;
    descriptor.https = &invalid_https;
    assert(mybot_platform_register(&descriptor) < 0);

    mybot_lcd_ops_t invalid_lcd = s_lcd;
    invalid_lcd.render = NULL;
    descriptor = complete_descriptor();
    descriptor.capabilities |= MYBOT_PLATFORM_CAP_LCD;
    descriptor.lcd = &invalid_lcd;
    assert(mybot_platform_register(&descriptor) < 0);

    mybot_announce_ops_t invalid_announce = s_announce;
    invalid_announce.read = NULL;
    descriptor = complete_descriptor();
    descriptor.capabilities |= MYBOT_PLATFORM_CAP_ANNOUNCE;
    descriptor.announce = &invalid_announce;
    assert(mybot_platform_register(&descriptor) < 0);

    mybot_wake_words_ops_t invalid_wake_words = s_wake_words;
    invalid_wake_words.process = NULL;
    descriptor = complete_descriptor();
    descriptor.capabilities |= MYBOT_PLATFORM_CAP_WAKE_WORDS;
    descriptor.wake_words = &invalid_wake_words;
    assert(mybot_platform_register(&descriptor) < 0);

    descriptor = complete_descriptor();
    descriptor.capabilities |= UINT64_C(1) << 63;
    assert(mybot_platform_register(&descriptor) < 0);

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
    assert(mybot_platform_register(&descriptor) < 0);
    return 0;
}
