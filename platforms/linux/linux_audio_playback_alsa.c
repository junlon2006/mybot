/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_audio.h>

#include "linux_platform_adapters.h"

#include <alsa/asoundlib.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <api/aosl_log.h>
#include <hal/aosl_hal_time.h>

/* Bounded wait for poll-based (non-blocking) PCM I/O. Keeps read/write
 * interruptible so worker threads can observe stop conditions and exit
 * promptly instead of blocking forever inside the driver. */
#define PCM_POLL_TIMEOUT_MS 50
#define PCM_RESUME_TIMEOUT_MS 500
#define PCM_RESUME_RETRY_MS 1

/* ---- ALSA error recovery helpers ---- */
static int xrun_recover(snd_pcm_t *handle) {
    snd_pcm_status_t *status;
    snd_pcm_status_alloca(&status);

    int err = snd_pcm_status(handle, status);
    if (err < 0) {
        AOSL_LOG_ERR("xrun_recover: status error: %s", snd_strerror(err));
        goto prepare;
    }
    if (snd_pcm_status_get_state(status) == SND_PCM_STATE_XRUN) {
        snd_pcm_uframes_t delay = snd_pcm_status_get_delay(status);
        AOSL_LOG_WRN("xrun: underrun detected (%lu frames), recovering", (unsigned long)delay);
    }

prepare:
    err = snd_pcm_prepare(handle);
    if (err < 0) {
        AOSL_LOG_ERR("xrun_recover: prepare failed: %s", snd_strerror(err));
        return -1;
    }
    return 0;
}

static int suspend_recover(snd_pcm_t *handle) {
    int err;
    uint64_t deadline = aosl_hal_get_tick_ms() + PCM_RESUME_TIMEOUT_MS;

    while ((err = snd_pcm_resume(handle)) == -EAGAIN) {
        if (aosl_hal_get_tick_ms() >= deadline) {
            break;
        }
        aosl_hal_msleep(PCM_RESUME_RETRY_MS);
    }
    if (err < 0) {
        err = snd_pcm_prepare(handle);
        if (err < 0) {
            AOSL_LOG_ERR("suspend_recover: resume/prepare failed: %s", snd_strerror(err));
            return -1;
        }
    }
    return 0;
}

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
            if (xrun_recover(handle) < 0) {
                return -1;
            }
        } else if (r == -ESTRPIPE) {
            if (suspend_recover(handle) < 0) {
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
    int rate;
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

    p->rate = rate;
    p->channels = channels;
    p->bits_per_sample = bits;

    AOSL_LOG_NTC("init: rate=%d channels=%d bits=%d", rate, channels, bits);

    int err;
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;

    err = snd_pcm_open(&p->handle, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        AOSL_LOG_ERR("init: snd_pcm_open failed: %s", snd_strerror(err));
        goto fail;
    }

    /* Non-blocking mode: pcm_write() uses snd_pcm_wait() with a timeout so a
     * playback thread blocked on a full buffer can still notice shutdown. */
    snd_pcm_nonblock(p->handle, 1);

    /* HW params */
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(p->handle, hw);
    snd_pcm_hw_params_set_access(p->handle, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(p->handle, hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(p->handle, hw, channels);

    unsigned int actual_rate = rate;
    err = snd_pcm_hw_params_set_rate_near(p->handle, hw, &actual_rate, 0);
    if (err < 0) {
        AOSL_LOG_ERR("init: set_rate_near failed: %s", snd_strerror(err));
        goto fail;
    }
    if (actual_rate != (unsigned int)rate) {
        /* The app derives frame sizes from the requested rate; a mismatch
         * would silently skew timing, so fail rather than run wrong. */
        AOSL_LOG_ERR("init: device rate %u != requested %d", actual_rate, rate);
        goto fail;
    }

    /* A 150 ms playback buffer absorbs device scheduling jitter. */
    snd_pcm_uframes_t buf_frames = (snd_pcm_uframes_t)(rate * 150 / 1000);
    snd_pcm_hw_params_set_buffer_size_near(p->handle, hw, &buf_frames);

    snd_pcm_uframes_t period_frames = buf_frames / 4;
    snd_pcm_hw_params_set_period_size_near(p->handle, hw, &period_frames, 0);

    err = snd_pcm_hw_params(p->handle, hw);
    if (err < 0) {
        AOSL_LOG_ERR("init: hw_params failed: %s", snd_strerror(err));
        goto fail;
    }

    AOSL_LOG_DBG("init: hw_params ok (rate=%u, buf=%lu, period=%lu)", actual_rate,
                 (unsigned long)buf_frames, (unsigned long)period_frames);

    /* SW params: start only when buffer is full */
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(p->handle, sw);
    snd_pcm_sw_params_set_avail_min(p->handle, sw, period_frames);
    snd_pcm_sw_params_set_start_threshold(p->handle, sw, buf_frames);
    err = snd_pcm_sw_params(p->handle, sw);
    if (err < 0) {
        AOSL_LOG_ERR("init: sw_params failed: %s", snd_strerror(err));
        goto fail;
    }

    *ctx = p;
    AOSL_LOG_NTC("init: ok");
    return 0;

fail:
    if (p->handle) {
        snd_pcm_close(p->handle);
    }
    free(p);
    return -1;
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
    .name = "alsa",
    .init = alsa_playback_init,
    .start = alsa_playback_start,
    .write = alsa_playback_write,
    .stop = alsa_playback_stop,
    .destroy = alsa_playback_destroy,
};

const mybot_audio_playback_ops_t *linux_audio_platform_alsa_playback_ops(void) {
    return &g_alsa_playback_ops;
}

int linux_audio_platform_register_alsa_playback(void) {
    int ret = mybot_audio_register_playback(&g_alsa_playback_ops);
    if (ret == 0) {
        AOSL_LOG_NTC("ALSA playback platform registered");
    }
    return ret;
}
