/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_platform_registry.h"
#include "platform_test.h"

#include <assert.h>

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
    return mybot_test_kv_init(connection);
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
    return mybot_test_kv_init(ctx);
}

static int wake_words_process(void *ctx, const void *pcm, int frames) {
    (void)ctx;
    (void)pcm;
    (void)frames;
    return 0;
}

static const mybot_lcd_ops_t s_lcd = {
    .init = mybot_test_kv_init,
    .render = lcd_render,
    .destroy = mybot_test_destroy,
};
static const mybot_audio_volume_ops_t s_volume = {
    .init = mybot_test_kv_init,
    .set_volume = volume_set,
    .destroy = mybot_test_destroy,
};
static const mybot_https_ops_t s_https = {
    .connect = https_connect,
    .send = https_send,
    .recv = https_recv,
    .close = mybot_test_destroy,
};
static const mybot_announce_ops_t s_announce = {
    .init = mybot_test_kv_init,
    .open = announce_open,
    .read = announce_read,
    .close = announce_close,
    .destroy = mybot_test_destroy,
};
static const mybot_wake_words_ops_t s_wake_words = {
    .init = wake_words_init,
    .process = wake_words_process,
    .destroy = mybot_test_destroy,
};
int main(void) {
    const mybot_platform_descriptor_t required = mybot_test_platform_descriptor();
    mybot_platform_descriptor_t descriptor;

    assert(mybot_platform_register(NULL) < 0);
    assert(!mybot_platform_registry_is_registered());

    descriptor = required;
    descriptor.key = NULL;
    assert(mybot_platform_register(&descriptor) < 0);
    assert(!mybot_platform_registry_is_registered());

    mybot_wifi_ops_t invalid_wifi = *required.wifi;
    invalid_wifi.init = NULL;
    descriptor = required;
    descriptor.wifi = &invalid_wifi;
    assert(mybot_platform_register(&descriptor) < 0);

    mybot_kv_store_ops_t invalid_kv = *required.kv_store;
    invalid_kv.get = NULL;
    descriptor = required;
    descriptor.kv_store = &invalid_kv;
    assert(mybot_platform_register(&descriptor) < 0);

    mybot_key_ops_t invalid_key = *required.key;
    invalid_key.init = NULL;
    descriptor = required;
    descriptor.key = &invalid_key;
    assert(mybot_platform_register(&descriptor) < 0);

    mybot_audio_capture_ops_t invalid_capture = *required.audio_capture;
    invalid_capture.read = NULL;
    descriptor = required;
    descriptor.audio_capture = &invalid_capture;
    assert(mybot_platform_register(&descriptor) < 0);

    mybot_audio_playback_ops_t invalid_playback = *required.audio_playback;
    invalid_playback.write = NULL;
    descriptor = required;
    descriptor.audio_playback = &invalid_playback;
    assert(mybot_platform_register(&descriptor) < 0);

#if MYBOT_ENABLE_VIDEO
    descriptor = required;
    descriptor.video = NULL;
    assert(mybot_platform_register(&descriptor) < 0);

    mybot_video_ops_t invalid_video = *required.video;
    invalid_video.on_target_bitrate_changed = NULL;
    descriptor = required;
    descriptor.video = &invalid_video;
    assert(mybot_platform_register(&descriptor) < 0);

    invalid_video = *required.video;
    invalid_video.max_bps = 0;
    descriptor = required;
    descriptor.video = &invalid_video;
    assert(mybot_platform_register(&descriptor) < 0);

    invalid_video = *required.video;
    invalid_video.min_bps = invalid_video.max_bps + 1U;
    descriptor = required;
    descriptor.video = &invalid_video;
    assert(mybot_platform_register(&descriptor) < 0);
#endif

    mybot_audio_volume_ops_t invalid_volume = s_volume;
    invalid_volume.set_volume = NULL;
    descriptor = required;
    descriptor.audio_volume = &invalid_volume;
    assert(mybot_platform_register(&descriptor) < 0);

    mybot_https_ops_t invalid_https = s_https;
    invalid_https.recv = NULL;
    descriptor = required;
    descriptor.https = &invalid_https;
    assert(mybot_platform_register(&descriptor) < 0);

    mybot_lcd_ops_t invalid_lcd = s_lcd;
    invalid_lcd.render = NULL;
    descriptor = required;
    descriptor.lcd = &invalid_lcd;
    assert(mybot_platform_register(&descriptor) < 0);

    mybot_announce_ops_t invalid_announce = s_announce;
    invalid_announce.read = NULL;
    descriptor = required;
    descriptor.announce = &invalid_announce;
    assert(mybot_platform_register(&descriptor) < 0);

    mybot_wake_words_ops_t invalid_wake_words = s_wake_words;
    invalid_wake_words.process = NULL;
    descriptor = required;
    descriptor.wake_words = &invalid_wake_words;
    assert(mybot_platform_register(&descriptor) < 0);

    descriptor = required;
    descriptor.audio_volume = &s_volume;
    descriptor.https = &s_https;
    descriptor.lcd = &s_lcd;
    descriptor.announce = &s_announce;
    descriptor.wake_words = &s_wake_words;
    assert(mybot_platform_register(&descriptor) == 0);
    assert(mybot_platform_registry_is_registered());
    const mybot_platform_descriptor_t *registered = mybot_platform_registry_get();
    assert(registered->wifi == required.wifi);
    assert(registered->kv_store == required.kv_store);
    assert(registered->key == required.key);
    assert(registered->audio_capture == required.audio_capture);
    assert(registered->audio_playback == required.audio_playback);
    assert(registered->audio_volume == &s_volume);
    assert(registered->https == &s_https);
    assert(registered->lcd == &s_lcd);
    assert(registered->announce == &s_announce);
    assert(registered->wake_words == &s_wake_words);
#if MYBOT_ENABLE_VIDEO
    assert(registered->video == required.video);
#endif

    /* Registration is process-wide and succeeds only once. */
    assert(mybot_platform_register(&descriptor) < 0);
    return 0;
}
