/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_audio.h>

#include "mybot_audio_internal.h"

#include <api/aosl_atomic.h>

#include <string.h>

/* Singleton registry: one capture, playback, and volume implementation may be registered. */
static const mybot_audio_capture_ops_t *g_capture_ops = NULL;
static const mybot_audio_playback_ops_t *g_playback_ops = NULL;
static const mybot_audio_volume_ops_t *g_volume_ops = NULL;
static void *g_volume_ctx = NULL;
static bool g_volume_active = false;

/* Media volume is SDK-managed digital gain. Defaults to unity (no scaling). */
static aosl_atomic_t g_media_volume = MYBOT_AUDIO_VOLUME_DEFAULT;
/* SDK-tracked device volume, used to step volume when the implementation has no
 * get_volume() and to resume after an implementation re-initialization. */
static aosl_atomic_t g_device_volume = MYBOT_AUDIO_VOLUME_DEFAULT;

int mybot_audio_register_capture(const mybot_audio_capture_ops_t *ops) {
    if (!ops || !ops->init || !ops->start || !ops->read || !ops->stop || !ops->destroy ||
        g_capture_ops) {
        return -1;
    }
    g_capture_ops = ops;
    return 0;
}

int mybot_audio_register_playback(const mybot_audio_playback_ops_t *ops) {
    if (!ops || !ops->init || !ops->start || !ops->write || !ops->stop || !ops->destroy ||
        g_playback_ops) {
        return -1;
    }
    g_playback_ops = ops;
    return 0;
}

const mybot_audio_capture_ops_t *mybot_audio_get_capture(void) {
    return g_capture_ops;
}

const mybot_audio_playback_ops_t *mybot_audio_get_playback(void) {
    return g_playback_ops;
}

int mybot_audio_device_register_volume(const mybot_audio_volume_ops_t *ops) {
    if (!ops || !ops->init || !ops->set_volume || !ops->destroy || g_volume_ops) {
        return -1;
    }
    g_volume_ops = ops;
    return 0;
}

bool mybot_audio_device_volume_is_registered(void) {
    return g_volume_ops != NULL;
}

bool mybot_audio_device_volume_is_active(void) {
    return g_volume_active;
}

int mybot_audio_device_volume_init(void) {
    if (g_volume_active || !g_volume_ops) {
        return -1;
    }
    if (g_volume_ops->init(&g_volume_ctx) < 0) {
        g_volume_ctx = NULL;
        return -1;
    }
    g_volume_active = true;
    /* Sync the SDK-tracked value with the implementation when it can report one. */
    if (g_volume_ops->get_volume) {
        int volume = 0;
        if (g_volume_ops->get_volume(g_volume_ctx, &volume) == 0) {
            aosl_atomic_set(&g_device_volume, volume);
        }
    }
    return 0;
}

void mybot_audio_device_volume_deinit(void) {
    if (!g_volume_active) {
        return;
    }
    g_volume_ops->destroy(g_volume_ctx);
    g_volume_ctx = NULL;
    g_volume_active = false;
}

int mybot_audio_device_set_volume(int volume) {
    if (volume < MYBOT_AUDIO_VOLUME_MIN || volume > MYBOT_AUDIO_VOLUME_MAX || !g_volume_active ||
        !g_volume_ops || !g_volume_ops->set_volume) {
        return -1;
    }
    if (g_volume_ops->set_volume(g_volume_ctx, volume) < 0) {
        return -1;
    }
    aosl_atomic_set(&g_device_volume, volume);
    return 0;
}

int mybot_audio_device_get_volume(int *volume) {
    if (!volume || !g_volume_active || !g_volume_ops) {
        return -1;
    }
    if (g_volume_ops->get_volume) {
        return g_volume_ops->get_volume(g_volume_ctx, volume);
    }
    *volume = (int)aosl_atomic_read(&g_device_volume);
    return 0;
}

int mybot_audio_set_media_volume(int volume) {
    if (volume < MYBOT_AUDIO_VOLUME_MIN || volume > MYBOT_AUDIO_VOLUME_MAX) {
        return -1;
    }
    aosl_atomic_set(&g_media_volume, volume);
    return 0;
}

int mybot_audio_get_media_volume(void) {
    return (int)aosl_atomic_read(&g_media_volume);
}

void mybot_audio_apply_media_volume(int16_t *pcm, int samples) {
    if (!pcm || samples <= 0) {
        return;
    }

    const int volume = (int)aosl_atomic_read(&g_media_volume);
    if (volume == MYBOT_AUDIO_VOLUME_MAX) {
        /* Unity gain — skip processing entirely. */
        return;
    }
    if (volume == MYBOT_AUDIO_VOLUME_MIN) {
        memset(pcm, 0, (size_t)samples * sizeof(*pcm));
        return;
    }

    /* 16.16 fixed-point linear amplitude gain: volume / 100. */
    const int32_t gain_q16 = ((int32_t)volume << 16) / MYBOT_AUDIO_VOLUME_MAX;
    for (int i = 0; i < samples; i++) {
        int32_t s = ((int32_t)pcm[i] * gain_q16 + 0x8000) >> 16;
        if (s > INT16_MAX) {
            s = INT16_MAX;
        } else if (s < INT16_MIN) {
            s = INT16_MIN;
        }
        pcm[i] = (int16_t)s;
    }
}
