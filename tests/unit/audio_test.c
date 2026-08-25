/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_audio.h>

#include "mybot_audio_internal.h"
#include "platform_test.h"

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
    mybot_audio_t audio = {0};
    const mybot_audio_capture_ops_t capture = {
        .init = init,
        .start = start,
        .read = read_pcm,
        .stop = stop,
        .destroy = destroy,
    };
    const mybot_audio_playback_ops_t playback = {
        .init = init,
        .start = start,
        .write = write_pcm,
        .stop = stop,
        .destroy = destroy,
    };
    const mybot_audio_volume_ops_t volume = {
        .init = volume_init,
        .set_volume = volume_set,
        .get_volume = volume_get,
        .destroy = volume_destroy,
    };
    const mybot_audio_volume_ops_t volume_without_get = {
        .init = volume_init,
        .set_volume = volume_set,
        .destroy = volume_destroy,
    };

    mybot_platform_descriptor_t descriptor = mybot_test_platform_descriptor();
    descriptor.audio_capture = &capture;
    descriptor.audio_playback = &playback;
    descriptor.audio_volume = &volume;
    assert(mybot_platform_register(&descriptor) == 0);

    mybot_audio_context_init(&audio);
    assert(audio.capture_ops == &capture);
    assert(audio.playback_ops == &playback);

    /* Device volume implementation registration and lifecycle. */
    assert(!mybot_audio_device_volume_is_active(&audio));

    int v = -1;
    assert(mybot_audio_device_set_volume(&audio, 50) < 0); /* implementation not initialized yet */
    assert(mybot_audio_device_get_volume(&audio, &v) < 0);

    assert(mybot_audio_device_volume_init(&audio) == 0);
    assert(mybot_audio_device_volume_is_active(&audio));
    assert(mybot_audio_device_volume_init(&audio) < 0); /* double init */
    assert(mybot_audio_device_set_volume(&audio, 60) == 0);
    assert(mybot_audio_device_get_volume(&audio, &v) == 0);
    assert(v == 60);
    assert(mybot_audio_device_set_volume(&audio, MYBOT_AUDIO_VOLUME_MIN - 1) < 0);
    assert(mybot_audio_device_set_volume(&audio, MYBOT_AUDIO_VOLUME_MAX + 1) < 0);
    assert(mybot_audio_device_get_volume(&audio, NULL) < 0);

    mybot_audio_device_volume_deinit(&audio);
    mybot_audio_device_volume_deinit(&audio); /* idempotent */
    assert(!mybot_audio_device_volume_is_active(&audio));
    assert(mybot_audio_device_set_volume(&audio, 70) < 0);
    assert(mybot_audio_device_get_volume(&audio, &v) < 0);
    assert(mybot_audio_device_volume_init(&audio) == 0); /* re-init after deinit */
    assert(mybot_audio_device_volume_is_active(&audio));
    assert(mybot_audio_device_get_volume(&audio, &v) == 0);
    assert(v == 60); /* implementation state survives deinit in this fake implementation */
    mybot_audio_device_volume_deinit(&audio);
    assert(!mybot_audio_device_volume_is_active(&audio));

    /* A platform without get_volume uses the SDK-tracked device value. */
    audio.volume_ops = &volume_without_get;
    assert(mybot_audio_device_volume_init(&audio) == 0);
    assert(mybot_audio_device_set_volume(&audio, 70) == 0);
    assert(mybot_audio_device_get_volume(&audio, &v) == 0);
    assert(v == 70);
    mybot_audio_device_volume_deinit(&audio);

    /* Media volume defaults to unity and skips processing. */
    assert(mybot_audio_get_media_volume(&audio) == MYBOT_AUDIO_VOLUME_DEFAULT);
    int16_t unity[] = {1000, -1000, INT16_MAX, INT16_MIN, 0};
    int16_t unity_expected[] = {1000, -1000, INT16_MAX, INT16_MIN, 0};
    mybot_audio_apply_media_volume(&audio, unity, 5);
    assert(memcmp(unity, unity_expected, sizeof(unity)) == 0);

    /* Media volume 0 silences the buffer. */
    assert(mybot_audio_set_media_volume(&audio, MYBOT_AUDIO_VOLUME_MIN) == 0);
    int16_t mute[] = {1000, -1000, 1, -1, 7};
    int16_t mute_expected[5] = {0};
    mybot_audio_apply_media_volume(&audio, mute, 5);
    assert(memcmp(mute, mute_expected, sizeof(mute)) == 0);

    /* Half media volume scales linearly (16.16 fixed point, rounded). */
    assert(mybot_audio_set_media_volume(&audio, 50) == 0);
    int16_t half[] = {1000, -1000, 10000, -10000, 0};
    int16_t half_expected[] = {500, -500, 5000, -5000, 0};
    mybot_audio_apply_media_volume(&audio, half, 5);
    assert(memcmp(half, half_expected, sizeof(half)) == 0);

    /* Media volume bounds. */
    assert(mybot_audio_set_media_volume(&audio, MYBOT_AUDIO_VOLUME_MIN - 1) < 0);
    assert(mybot_audio_set_media_volume(&audio, MYBOT_AUDIO_VOLUME_MAX + 1) < 0);
    assert(mybot_audio_get_media_volume(&audio) == 50);

    /* Guard against invalid inputs. */
    mybot_audio_apply_media_volume(&audio, NULL, 5);
    mybot_audio_apply_media_volume(&audio, half_expected, 0);
    mybot_audio_apply_media_volume(&audio, half_expected, -1);
    assert(memcmp(half, half_expected, sizeof(half)) == 0);

    assert(mybot_audio_set_media_volume(&audio, MYBOT_AUDIO_VOLUME_MAX) == 0);
    return 0;
}
