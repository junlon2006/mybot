/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_audio.h>

#include "mybot_audio_internal.h"
#include "mybot_platform_registry.h"

#include <api/aosl_atomic.h>

#include <string.h>

void mybot_audio_context_init(mybot_audio_t *audio) {
    if (!audio) {
        return;
    }
    memset(audio, 0, sizeof(*audio));
    const mybot_platform_descriptor_t *platform = mybot_platform_registry_get();
    audio->capture_ops = platform->audio_capture;
    audio->playback_ops = platform->audio_playback;
    audio->volume_ops = platform->audio_volume;
    audio->device_volume = MYBOT_AUDIO_VOLUME_DEFAULT;
    aosl_atomic_set(&audio->media_volume, MYBOT_AUDIO_VOLUME_DEFAULT);
}

bool mybot_audio_device_volume_is_active(const mybot_audio_t *audio) {
    return audio && audio->volume_active;
}

int mybot_audio_device_volume_init(mybot_audio_t *audio) {
    if (!audio || audio->volume_active || !audio->volume_ops) {
        return -1;
    }
    if (audio->volume_ops->init(&audio->volume_ctx) < 0) {
        audio->volume_ctx = NULL;
        return -1;
    }
    audio->volume_active = true;
    /* Sync the SDK-tracked value with the implementation when it can report one. */
    if (audio->volume_ops->get_volume) {
        int volume = 0;
        if (audio->volume_ops->get_volume(audio->volume_ctx, &volume) == 0) {
            audio->device_volume = volume;
        }
    }
    return 0;
}

void mybot_audio_device_volume_deinit(mybot_audio_t *audio) {
    if (!audio || !audio->volume_active) {
        return;
    }
    audio->volume_ops->destroy(audio->volume_ctx);
    audio->volume_ctx = NULL;
    audio->volume_active = false;
}

int mybot_audio_device_set_volume(mybot_audio_t *audio, int volume) {
    if (!audio || volume < MYBOT_AUDIO_VOLUME_MIN || volume > MYBOT_AUDIO_VOLUME_MAX ||
        !audio->volume_active || !audio->volume_ops || !audio->volume_ops->set_volume) {
        return -1;
    }
    if (audio->volume_ops->set_volume(audio->volume_ctx, volume) < 0) {
        return -1;
    }
    audio->device_volume = volume;
    return 0;
}

int mybot_audio_device_get_volume(mybot_audio_t *audio, int *volume) {
    if (!audio || !volume || !audio->volume_active || !audio->volume_ops) {
        return -1;
    }
    if (audio->volume_ops->get_volume) {
        return audio->volume_ops->get_volume(audio->volume_ctx, volume);
    }
    *volume = audio->device_volume;
    return 0;
}

int mybot_audio_set_media_volume(mybot_audio_t *audio, int volume) {
    if (!audio || volume < MYBOT_AUDIO_VOLUME_MIN || volume > MYBOT_AUDIO_VOLUME_MAX) {
        return -1;
    }
    aosl_atomic_set(&audio->media_volume, volume);
    return 0;
}

int mybot_audio_get_media_volume(const mybot_audio_t *audio) {
    return audio ? (int)aosl_atomic_read(&audio->media_volume) : MYBOT_AUDIO_VOLUME_DEFAULT;
}

void mybot_audio_apply_media_volume(const mybot_audio_t *audio, int16_t *pcm, int samples) {
    if (!audio || !pcm || samples <= 0) {
        return;
    }

    const int volume = (int)aosl_atomic_read(&audio->media_volume);
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
