#include "audio_device.h"

/* Singleton registry — only one capture and one playback platform active. */
static const audio_capture_ops_t  *g_capture_ops  = NULL;
static const audio_playback_ops_t *g_playback_ops = NULL;

int audio_device_register_capture(const audio_capture_ops_t *ops)
{
    if (!ops || !ops->init || !ops->read || !ops->destroy)
        return -1;
    g_capture_ops = ops;
    return 0;
}

int audio_device_register_playback(const audio_playback_ops_t *ops)
{
    if (!ops || !ops->init || !ops->write || !ops->destroy)
        return -1;
    g_playback_ops = ops;
    return 0;
}

const audio_capture_ops_t *audio_device_get_capture(void)
{
    return g_capture_ops;
}

const audio_playback_ops_t *audio_device_get_playback(void)
{
    return g_playback_ops;
}
