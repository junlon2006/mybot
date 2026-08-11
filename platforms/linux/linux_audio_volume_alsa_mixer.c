/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_audio.h>

#include "linux_platform_adapters.h"

#include <alsa/asoundlib.h>

#include <stdlib.h>

#include <api/aosl_log.h>

/* Fallback chain for the real-device volume control element. "Master" is the
 * standard control on most sound cards; PCM and Digital are common
 * alternatives. The first element that exists is used. */
static const char *const k_volume_elements[] = {"Master", "PCM", "Digital"};

typedef struct {
    snd_mixer_t *mixer;
    snd_mixer_elem_t *elem;
    long min;
    long max;
} alsa_volume_t;

static snd_mixer_elem_t *find_volume_element(snd_mixer_t *mixer) {
    snd_mixer_selem_id_t *sid;
    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);

    for (size_t i = 0; i < sizeof(k_volume_elements) / sizeof(k_volume_elements[0]); i++) {
        snd_mixer_selem_id_set_name(sid, k_volume_elements[i]);
        snd_mixer_elem_t *elem = snd_mixer_find_selem(mixer, sid);
        if (elem) {
            return elem;
        }
    }
    return NULL;
}

static int alsa_volume_init(void **ctx) {
    if (!ctx) {
        return -1;
    }

    alsa_volume_t *v = (alsa_volume_t *)calloc(1, sizeof(alsa_volume_t));
    if (!v) {
        return -1;
    }

    int err = snd_mixer_open(&v->mixer, 0);
    if (err < 0) {
        AOSL_LOG_ERR("volume init: snd_mixer_open failed: %s", snd_strerror(err));
        goto fail;
    }
    err = snd_mixer_attach(v->mixer, "default");
    if (err < 0) {
        AOSL_LOG_ERR("volume init: snd_mixer_attach failed: %s", snd_strerror(err));
        goto fail;
    }
    err = snd_mixer_selem_register(v->mixer, NULL, NULL);
    if (err < 0) {
        AOSL_LOG_ERR("volume init: snd_mixer_selem_register failed: %s", snd_strerror(err));
        goto fail;
    }
    err = snd_mixer_load(v->mixer);
    if (err < 0) {
        AOSL_LOG_ERR("volume init: snd_mixer_load failed: %s", snd_strerror(err));
        goto fail;
    }

    v->elem = find_volume_element(v->mixer);
    if (!v->elem) {
        AOSL_LOG_WRN("volume init: no Master/PCM/Digital playback control found");
        goto fail;
    }
    snd_mixer_selem_get_playback_volume_range(v->elem, &v->min, &v->max);
    if (v->max <= v->min) {
        AOSL_LOG_ERR("volume init: empty playback volume range");
        goto fail;
    }

    *ctx = v;
    AOSL_LOG_INF("volume init: ok (element=%s, range=%ld..%ld)", snd_mixer_selem_get_name(v->elem),
                 v->min, v->max);
    return 0;

fail:
    if (v->mixer) {
        snd_mixer_close(v->mixer);
    }
    free(v);
    return -1;
}

static int alsa_volume_set(void *ctx, int volume) {
    alsa_volume_t *v = (alsa_volume_t *)ctx;
    if (!v || !v->elem) {
        return -1;
    }

    const long span = v->max - v->min;
    long value =
        v->min + ((long)volume * span + MYBOT_AUDIO_VOLUME_MAX / 2) / MYBOT_AUDIO_VOLUME_MAX;
    if (snd_mixer_selem_set_playback_volume_all(v->elem, value) < 0) {
        AOSL_LOG_ERR("volume set: snd_mixer_selem_set_playback_volume_all failed");
        return -1;
    }
    return 0;
}

static int alsa_volume_get(void *ctx, int *volume) {
    alsa_volume_t *v = (alsa_volume_t *)ctx;
    if (!v || !v->elem || !volume) {
        return -1;
    }

    long value = 0;
    if (snd_mixer_selem_get_playback_volume(v->elem, SND_MIXER_SCHN_FRONT_LEFT, &value) < 0) {
        AOSL_LOG_ERR("volume get: snd_mixer_selem_get_playback_volume failed");
        return -1;
    }

    const long span = v->max - v->min;
    *volume = (int)(((value - v->min) * MYBOT_AUDIO_VOLUME_MAX + span / 2) / span);
    return 0;
}

static void alsa_volume_destroy(void *ctx) {
    if (!ctx) {
        return;
    }
    alsa_volume_t *v = (alsa_volume_t *)ctx;
    if (v->mixer) {
        snd_mixer_close(v->mixer);
    }
    free(v);
}

static const mybot_audio_volume_ops_t g_alsa_volume_ops = {
    .name = "alsa-mixer",
    .init = alsa_volume_init,
    .set_volume = alsa_volume_set,
    .get_volume = alsa_volume_get,
    .destroy = alsa_volume_destroy,
};

int linux_audio_platform_register_alsa_volume(void) {
    int ret = mybot_audio_device_register_volume(&g_alsa_volume_ops);
    if (ret == 0) {
        AOSL_LOG_INF("ALSA volume platform registered");
    }
    return ret;
}
