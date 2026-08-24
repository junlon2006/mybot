/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_TEST_PLATFORM_H_
#define MYBOT_TEST_PLATFORM_H_

#include <mybot/platform/mybot_platform.h>

static int mybot_test_wifi_init(void **ctx, const char *device_id,
                                mybot_wifi_event_handler_t handler, void *user_data) {
    (void)device_id;
    (void)handler;
    (void)user_data;
    *ctx = ctx;
    return 0;
}

static void mybot_test_destroy(void *ctx) {
    (void)ctx;
}

static int mybot_test_kv_init(void **ctx) {
    *ctx = ctx;
    return 0;
}

static int mybot_test_kv_get(void *ctx, const char *key, void *value, size_t capacity,
                             size_t *out_len) {
    (void)ctx;
    (void)key;
    (void)value;
    (void)capacity;
    (void)out_len;
    return -1;
}

static int mybot_test_kv_set(void *ctx, const char *key, const void *value, size_t len) {
    (void)ctx;
    (void)key;
    (void)value;
    (void)len;
    return 0;
}

static int mybot_test_kv_erase(void *ctx, const char *key) {
    (void)ctx;
    (void)key;
    return 0;
}

static int mybot_test_key_init(void **ctx, mybot_key_event_handler_t handler, void *user_data) {
    (void)handler;
    (void)user_data;
    *ctx = ctx;
    return 0;
}

static int mybot_test_audio_init(void **ctx, int rate, int channels, int bits) {
    (void)rate;
    (void)channels;
    (void)bits;
    *ctx = ctx;
    return 0;
}

static int mybot_test_audio_start_stop(void *ctx) {
    (void)ctx;
    return 0;
}

static int mybot_test_audio_read(void *ctx, void *data, int frames) {
    (void)ctx;
    (void)data;
    return frames;
}

static int mybot_test_audio_write(void *ctx, const void *data, int frames) {
    (void)ctx;
    (void)data;
    return frames;
}

static inline mybot_platform_descriptor_t mybot_test_platform_descriptor(void) {
    static const mybot_wifi_ops_t wifi = {
        .name = "test-wifi",
        .init = mybot_test_wifi_init,
        .destroy = mybot_test_destroy,
    };
    static const mybot_kv_store_ops_t kv_store = {
        .name = "test-kv",
        .init = mybot_test_kv_init,
        .get = mybot_test_kv_get,
        .set = mybot_test_kv_set,
        .erase = mybot_test_kv_erase,
        .destroy = mybot_test_destroy,
    };
    static const mybot_key_ops_t key = {
        .name = "test-key",
        .init = mybot_test_key_init,
        .destroy = mybot_test_destroy,
    };
    static const mybot_audio_capture_ops_t capture = {
        .name = "test-capture",
        .init = mybot_test_audio_init,
        .start = mybot_test_audio_start_stop,
        .read = mybot_test_audio_read,
        .stop = mybot_test_audio_start_stop,
        .destroy = mybot_test_destroy,
    };
    static const mybot_audio_playback_ops_t playback = {
        .name = "test-playback",
        .init = mybot_test_audio_init,
        .start = mybot_test_audio_start_stop,
        .write = mybot_test_audio_write,
        .stop = mybot_test_audio_start_stop,
        .destroy = mybot_test_destroy,
    };

    mybot_platform_descriptor_t descriptor = {
        .api_version = MYBOT_PLATFORM_API_VERSION,
        .struct_size = sizeof(descriptor),
        .name = "test-platform",
        .capabilities = MYBOT_PLATFORM_CAP_REQUIRED,
        .wifi = &wifi,
        .kv_store = &kv_store,
        .key = &key,
        .audio_capture = &capture,
        .audio_playback = &playback,
    };
    return descriptor;
}

#endif /* MYBOT_TEST_PLATFORM_H_ */
