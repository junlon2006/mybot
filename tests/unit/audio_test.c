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
static int s_volume_init_result;
static int s_volume_set_result;
static int s_volume_get_result;
static int s_volume_destroy_count;

static int volume_init(void **ctx) {
    *ctx = s_volume_init_result == 0 ? &s_device_volume : NULL;
    return s_volume_init_result;
}

static int volume_set(void *ctx, int volume) {
    assert(ctx == &s_device_volume);
    if (s_volume_set_result < 0) {
        return s_volume_set_result;
    }
    s_device_volume = volume;
    return 0;
}

static int volume_get(void *ctx, int *volume) {
    assert(ctx == &s_device_volume);
    if (s_volume_get_result < 0) {
        return s_volume_get_result;
    }
    *volume = s_device_volume;
    return 0;
}

static void volume_destroy(void *ctx) {
    assert(ctx == &s_device_volume);
    s_volume_destroy_count++;
}

static void test_volume_failures(void) {
    mybot_audio_t audio = {0};
    mybot_audio_context_init(&audio);
    int destroys = s_volume_destroy_count;
    s_volume_init_result = -1;
    assert(mybot_audio_device_volume_init(&audio) == -1);
    assert(!mybot_audio_device_volume_is_active(&audio));
    assert(audio.volume_ctx == NULL);
    mybot_audio_device_volume_deinit(&audio);
    assert(s_volume_destroy_count == destroys);

    /* Hardware may initialize while its first volume read fails; the SDK keeps
     * its default tracked setting until a successful set. */
    s_volume_init_result = 0;
    s_volume_get_result = -1;
    assert(mybot_audio_device_volume_init(&audio) == 0);
    assert(mybot_audio_device_volume_is_active(&audio));
    assert(audio.device_volume == MYBOT_AUDIO_VOLUME_DEFAULT);
    int current = -1;
    assert(mybot_audio_device_get_volume(&audio, &current) == -1);
    assert(current == -1);
    assert(mybot_audio_device_set_volume(&audio, 45) == 0);
    assert(audio.device_volume == 45);

    s_volume_set_result = -1;
    assert(mybot_audio_device_set_volume(&audio, 80) == -1);
    assert(audio.device_volume == 45);
    assert(s_device_volume == 45);
    s_volume_set_result = 0;
    s_volume_get_result = 0;
    assert(mybot_audio_device_get_volume(&audio, &current) == 0);
    assert(current == 45);
    assert(mybot_audio_device_set_volume(&audio, MYBOT_AUDIO_VOLUME_MIN) == 0);
    assert(mybot_audio_device_get_volume(&audio, &current) == 0);
    assert(current == MYBOT_AUDIO_VOLUME_MIN);
    assert(mybot_audio_device_set_volume(&audio, MYBOT_AUDIO_VOLUME_MAX) == 0);
    assert(mybot_audio_device_get_volume(&audio, &current) == 0);
    assert(current == MYBOT_AUDIO_VOLUME_MAX);
    mybot_audio_device_volume_deinit(&audio);
    assert(s_volume_destroy_count == destroys + 1);
}

int main(void) {
    mybot_audio_t audio = {0};
    /* Without a registered hardware-volume implementation, software gain
     * remains available. */
    mybot_audio_context_init(&audio);
    assert(audio.volume_ops == NULL);
    assert(mybot_audio_device_volume_init(&audio) == -1);
    assert(mybot_audio_set_media_volume(&audio, 50) == 0);
    assert(mybot_audio_get_media_volume(&audio) == 50);

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

    test_volume_failures();
    mybot_audio_context_init(NULL);
    assert(!mybot_audio_device_volume_is_active(NULL));
    assert(mybot_audio_device_volume_init(NULL) == -1);
    mybot_audio_device_volume_deinit(NULL);
    assert(mybot_audio_device_set_volume(NULL, 50) == -1);
    assert(mybot_audio_device_get_volume(NULL, &v) == -1);
    assert(mybot_audio_set_media_volume(NULL, 50) == -1);
    assert(mybot_audio_get_media_volume(NULL) == MYBOT_AUDIO_VOLUME_DEFAULT);
    mybot_audio_apply_media_volume(NULL, half, 5);
    assert(memcmp(half, half_expected, sizeof(half)) == 0);

    return 0;
}
