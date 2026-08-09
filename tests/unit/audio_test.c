/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_audio.h>

#include "mybot_audio_internal.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

static int init(void **ctx, int rate, int channels, int bits) {
    (void)rate;
    (void)channels;
    (void)bits;
    *ctx = ctx;
    return 0;
}

static int start(void *ctx) {
    (void)ctx;
    return 0;
}

static int read_pcm(void *ctx, void *buf, int frames) {
    (void)ctx;
    (void)buf;
    return frames;
}

static int write_pcm(void *ctx, const void *buf, int frames) {
    (void)ctx;
    (void)buf;
    return frames;
}

static int stop(void *ctx) {
    (void)ctx;
    return 0;
}

static void destroy(void *ctx) {
    (void)ctx;
}

static int s_device_volume = MYBOT_AUDIO_VOLUME_DEFAULT;

static int volume_init(void **ctx) {
    *ctx = ctx;
    return 0;
}

static int volume_set(void *ctx, int volume) {
    (void)ctx;
    s_device_volume = volume;
    return 0;
}

static int volume_get(void *ctx, int *volume) {
    (void)ctx;
    *volume = s_device_volume;
    return 0;
}

static void volume_destroy(void *ctx) {
    (void)ctx;
}

int main(void) {
    const mybot_audio_capture_ops_t incomplete_capture = {0};
    const mybot_audio_playback_ops_t incomplete_playback = {0};
    const mybot_audio_capture_ops_t capture = {
        .name = "test",
        .init = init,
        .start = start,
        .read = read_pcm,
        .stop = stop,
        .destroy = destroy,
    };
    const mybot_audio_playback_ops_t playback = {
        .name = "test",
        .init = init,
        .start = start,
        .write = write_pcm,
        .stop = stop,
        .destroy = destroy,
    };
    const mybot_audio_volume_ops_t incomplete_volume = {0};
    const mybot_audio_volume_ops_t volume = {
        .name = "test",
        .init = volume_init,
        .set_volume = volume_set,
        .get_volume = volume_get,
        .destroy = volume_destroy,
    };

    assert(mybot_audio_register_capture(NULL) < 0);
    assert(mybot_audio_register_capture(&incomplete_capture) < 0);
    assert(mybot_audio_register_capture(&capture) == 0);
    assert(mybot_audio_register_capture(&capture) < 0);
    assert(mybot_audio_get_capture() == &capture);

    assert(mybot_audio_register_playback(NULL) < 0);
    assert(mybot_audio_register_playback(&incomplete_playback) < 0);
    assert(mybot_audio_register_playback(&playback) == 0);
    assert(mybot_audio_register_playback(&playback) < 0);
    assert(mybot_audio_get_playback() == &playback);

    /* Device volume backend registration and lifecycle. */
    assert(mybot_audio_device_register_volume(NULL) < 0);
    assert(mybot_audio_device_register_volume(&incomplete_volume) < 0);
    assert(mybot_audio_device_register_volume(&volume) == 0);
    assert(mybot_audio_device_register_volume(&volume) < 0);
    assert(mybot_audio_device_volume_is_registered());
    assert(!mybot_audio_device_volume_is_active());

    int v = -1;
    assert(mybot_audio_device_set_volume(50) < 0); /* backend not initialized yet */
    assert(mybot_audio_device_get_volume(&v) < 0);

    assert(mybot_audio_device_volume_init() == 0);
    assert(mybot_audio_device_volume_is_active());
    assert(mybot_audio_device_volume_init() < 0); /* double init */
    assert(mybot_audio_device_set_volume(60) == 0);
    assert(mybot_audio_device_get_volume(&v) == 0);
    assert(v == 60);
    assert(mybot_audio_device_set_volume(MYBOT_AUDIO_VOLUME_MIN - 1) < 0);
    assert(mybot_audio_device_set_volume(MYBOT_AUDIO_VOLUME_MAX + 1) < 0);
    assert(mybot_audio_device_get_volume(NULL) < 0);

    mybot_audio_device_volume_deinit();
    mybot_audio_device_volume_deinit(); /* idempotent */
    assert(!mybot_audio_device_volume_is_active());
    assert(mybot_audio_device_set_volume(70) < 0);
    assert(mybot_audio_device_get_volume(&v) < 0);
    assert(mybot_audio_device_volume_init() == 0); /* re-init after deinit */
    assert(mybot_audio_device_volume_is_active());
    assert(mybot_audio_device_get_volume(&v) == 0);
    assert(v == 60); /* backend state survives deinit in this fake backend */
    mybot_audio_device_volume_deinit();
    assert(!mybot_audio_device_volume_is_active());

    /* Media volume defaults to unity and skips processing. */
    assert(mybot_audio_get_media_volume() == MYBOT_AUDIO_VOLUME_DEFAULT);
    int16_t unity[] = {1000, -1000, INT16_MAX, INT16_MIN, 0};
    int16_t unity_expected[] = {1000, -1000, INT16_MAX, INT16_MIN, 0};
    mybot_audio_apply_media_volume(unity, 5);
    assert(memcmp(unity, unity_expected, sizeof(unity)) == 0);

    /* Media volume 0 silences the buffer. */
    assert(mybot_audio_set_media_volume(MYBOT_AUDIO_VOLUME_MIN) == 0);
    int16_t mute[] = {1000, -1000, 1, -1, 7};
    int16_t mute_expected[5] = {0};
    mybot_audio_apply_media_volume(mute, 5);
    assert(memcmp(mute, mute_expected, sizeof(mute)) == 0);

    /* Half media volume scales linearly (16.16 fixed point, rounded). */
    assert(mybot_audio_set_media_volume(50) == 0);
    int16_t half[] = {1000, -1000, 10000, -10000, 0};
    int16_t half_expected[] = {500, -500, 5000, -5000, 0};
    mybot_audio_apply_media_volume(half, 5);
    assert(memcmp(half, half_expected, sizeof(half)) == 0);

    /* Media volume bounds. */
    assert(mybot_audio_set_media_volume(MYBOT_AUDIO_VOLUME_MIN - 1) < 0);
    assert(mybot_audio_set_media_volume(MYBOT_AUDIO_VOLUME_MAX + 1) < 0);
    assert(mybot_audio_get_media_volume() == 50);

    /* Guard against invalid inputs. */
    mybot_audio_apply_media_volume(NULL, 5);
    mybot_audio_apply_media_volume(half_expected, 0);
    mybot_audio_apply_media_volume(half_expected, -1);
    assert(memcmp(half, half_expected, sizeof(half)) == 0);

    assert(mybot_audio_set_media_volume(MYBOT_AUDIO_VOLUME_MAX) == 0);
    return 0;
}
