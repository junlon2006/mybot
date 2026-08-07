#include <mybot/platform/mybot_audio.h>

/* Singleton registry — only one capture and one playback platform active. */
static const mybot_audio_capture_ops_t *g_capture_ops = NULL;
static const mybot_audio_playback_ops_t *g_playback_ops = NULL;

int mybot_audio_device_register_capture(const mybot_audio_capture_ops_t *ops) {
    if (!ops || !ops->init || !ops->read || !ops->destroy) {
        return -1;
    }
    g_capture_ops = ops;
    return 0;
}

int mybot_audio_device_register_playback(const mybot_audio_playback_ops_t *ops) {
    if (!ops || !ops->init || !ops->write || !ops->destroy) {
        return -1;
    }
    g_playback_ops = ops;
    return 0;
}

const mybot_audio_capture_ops_t *mybot_audio_device_get_capture(void) {
    return g_capture_ops;
}

const mybot_audio_playback_ops_t *mybot_audio_device_get_playback(void) {
    return g_playback_ops;
}
