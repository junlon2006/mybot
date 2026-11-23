/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_audio.h>

#include "linux_alsa_pcm.h"
#include "linux_platform_adapters.h"

#include <stdlib.h>

#include <api/aosl_log.h>

static int pcm_write(snd_pcm_t *handle, const char *buf, size_t frames, size_t frame_bytes) {
    ssize_t r;
    size_t count = frames;
    size_t result = 0;

    while (count > 0) {
        r = snd_pcm_writei(handle, buf + result * frame_bytes, count);
        if (r == -EAGAIN) {
            /* Buffer full right now — wait up to the poll timeout, then give
             * up so the caller can check its own stop condition. */
            if (snd_pcm_wait(handle, PCM_POLL_TIMEOUT_MS) <= 0) {
                break;
            }
        } else if (r == -EPIPE) {
            if (linux_alsa_recover_xrun(handle, "underrun") < 0) {
                return -1;
            }
        } else if (r == -ESTRPIPE) {
            if (linux_alsa_recover_suspend(handle) < 0) {
                return -1;
            }
        } else if (r < 0) {
            AOSL_LOG_ERR("pcm_write: %s", snd_strerror(r));
            return -1;
        } else if (r > 0) {
            result += (size_t)r;
            count -= (size_t)r;
        } else {
            break;
        }
    }
    return (int)result;
}

/* ---- Internal context ---- */
typedef struct {
    snd_pcm_t *handle;
    int channels;
    int bits_per_sample;
} alsa_pb_t;

/* ---- Playback ops implementation ---- */

static int alsa_playback_init(void **ctx, int rate, int channels, int bits) {
    if (!ctx) {
        return -1;
    }

    alsa_pb_t *p = (alsa_pb_t *)calloc(1, sizeof(alsa_pb_t));
    if (!p) {
        return -1;
    }

    p->channels = channels;
    p->bits_per_sample = bits;

    AOSL_LOG_NTC("init: rate=%d channels=%d bits=%d", rate, channels, bits);

    p->handle =
        linux_alsa_open_pcm(SND_PCM_STREAM_PLAYBACK, rate, channels, 150, PCM_START_ON_FULL_BUFFER);
    if (!p->handle) {
        free(p);
        return -1;
    }
    *ctx = p;
    AOSL_LOG_NTC("init: ok");
    return 0;
}

static int alsa_playback_start(void *ctx) {
    (void)ctx;
    AOSL_LOG_NTC("start");
    return 0;
}

static int alsa_playback_write(void *ctx, const void *buf, int frames) {
    alsa_pb_t *p = (alsa_pb_t *)ctx;
    size_t frame_bytes = (size_t)(p->bits_per_sample / 8 * p->channels);
    return pcm_write(p->handle, (const char *)buf, (size_t)frames, frame_bytes);
}

static int alsa_playback_stop(void *ctx) {
    AOSL_LOG_NTC("stop");
    if (ctx) {
        alsa_pb_t *p = (alsa_pb_t *)ctx;
        /* Immediate stop (does not wait for playback to drain), safe to call
         * from another thread than the one inside snd_pcm_writei. */
        snd_pcm_drop(p->handle);
    }
    return 0;
}

static void alsa_playback_destroy(void *ctx) {
    AOSL_LOG_NTC("destroy");
    if (!ctx) {
        return;
    }
    alsa_pb_t *p = (alsa_pb_t *)ctx;
    if (p->handle) {
        snd_pcm_drop(p->handle);
        snd_pcm_close(p->handle);
    }
    free(p);
}

static const mybot_audio_playback_ops_t g_alsa_playback_ops = {
    .init = alsa_playback_init,
    .start = alsa_playback_start,
    .write = alsa_playback_write,
    .stop = alsa_playback_stop,
    .destroy = alsa_playback_destroy,
};

const mybot_audio_playback_ops_t *linux_audio_platform_alsa_playback_ops(void) {
    return &g_alsa_playback_ops;
}
