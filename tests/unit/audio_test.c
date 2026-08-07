#include <mybot/platform/mybot_audio.h>

#include <assert.h>
#include <stddef.h>

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

    assert(mybot_audio_device_register_capture(NULL) < 0);
    assert(mybot_audio_device_register_capture(&incomplete_capture) < 0);
    assert(mybot_audio_device_register_capture(&capture) == 0);
    assert(mybot_audio_device_register_capture(&capture) < 0);
    assert(mybot_audio_device_get_capture() == &capture);

    assert(mybot_audio_device_register_playback(NULL) < 0);
    assert(mybot_audio_device_register_playback(&incomplete_playback) < 0);
    assert(mybot_audio_device_register_playback(&playback) == 0);
    assert(mybot_audio_device_register_playback(&playback) < 0);
    assert(mybot_audio_device_get_playback() == &playback);
    return 0;
}
