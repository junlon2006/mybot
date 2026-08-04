#include "audio_device.h"

#include <alsa/asoundlib.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <api/aosl_log.h>

/* 16 kHz, 16-bit, mono → 20 ms = 640 bytes / frame */
#define PCM_FRAMES_20MS  320
#define PCM_BYTES_20MS   640

/* Bounded wait for poll-based (non-blocking) PCM I/O. Keeps read/write
 * interruptible so worker threads can observe stop conditions and exit
 * promptly instead of blocking forever inside the driver. */
#define PCM_POLL_TIMEOUT_MS  50

/* Internal context */
typedef struct {
    snd_pcm_t      *handle;
    int             rate;
    int             channels;
    int             bits_per_sample;

    uint8_t         acc_buf[PCM_BYTES_20MS];
    int             acc_len;
} alsa_cap_t;

/* ---- ALSA error recovery helpers ---- */
static int xrun_recover(snd_pcm_t *handle)
{
    snd_pcm_status_t *status;
    snd_pcm_status_alloca(&status);

    int err = snd_pcm_status(handle, status);
    if (err < 0) {
        AOSL_LOG_ERR("xrun_recover: status error: %s", snd_strerror(err));
        goto prepare;
    }
    if (snd_pcm_status_get_state(status) == SND_PCM_STATE_XRUN) {
        snd_pcm_uframes_t delay = snd_pcm_status_get_delay(status);
        AOSL_LOG_WRN("xrun: overrun detected (%lu frames), recovering", (unsigned long)delay);
    }

prepare:
    err = snd_pcm_prepare(handle);
    if (err < 0) {
        AOSL_LOG_ERR("xrun_recover: prepare failed: %s", snd_strerror(err));
        return -1;
    }
    return 0;
}

static int suspend_recover(snd_pcm_t *handle)
{
    int err;
    while ((err = snd_pcm_resume(handle)) == -EAGAIN) {
        usleep(1000);
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

static int pcm_read(snd_pcm_t *handle, char *buf, size_t frames)
{
    ssize_t r;
    size_t  count = frames;
    size_t  result = 0;
    int     frame_bytes = 2;

    while (count > 0) {
        r = snd_pcm_readi(handle, buf + result * frame_bytes, count);
        if (r == -EAGAIN) {
            /* No data right now — wait up to the poll timeout, then give up
             * so the caller can check its own stop condition. */
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
            AOSL_LOG_ERR("pcm_read: %s", snd_strerror(r));
            return -1;
        } else if (r > 0) {
            result += (size_t)r;
            count  -= (size_t)r;
            if (count > 0 && snd_pcm_wait(handle, PCM_POLL_TIMEOUT_MS) <= 0) {
                break;   /* partial read; return what we have */
            }
        } else {
            break;
        }
    }
    return (int)result;
}

/* ---- capture ops implementation ---- */

static int alsa_capture_init(void **ctx, int rate, int channels, int bits)
{
    if (!ctx) {
        return -1;
    }

    alsa_cap_t *c = (alsa_cap_t *)calloc(1, sizeof(alsa_cap_t));
    if (!c) {
        return -1;
    }

    c->rate            = rate;
    c->channels        = channels;
    c->bits_per_sample = bits;

    AOSL_LOG_INF("init: rate=%d channels=%d bits=%d", rate, channels, bits);

    int err;
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;

    err = snd_pcm_open(&c->handle, "default", SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        AOSL_LOG_ERR("init: snd_pcm_open failed: %s", snd_strerror(err));
        goto fail;
    }

    /* Non-blocking mode: pcm_read() uses snd_pcm_wait() with a timeout so a
     * capture thread blocked on no data can still notice shutdown. */
    snd_pcm_nonblock(c->handle, 1);

    /* HW params */
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(c->handle, hw);
    snd_pcm_hw_params_set_access(c->handle, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(c->handle, hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(c->handle, hw, channels);

    unsigned int actual_rate = rate;
    snd_pcm_hw_params_set_rate_near(c->handle, hw, &actual_rate, 0);

    snd_pcm_uframes_t buf_frames = (snd_pcm_uframes_t)(rate * 50 / 1000);
    snd_pcm_hw_params_set_buffer_size_near(c->handle, hw, &buf_frames);

    snd_pcm_uframes_t period_frames = buf_frames / 4;
    snd_pcm_hw_params_set_period_size_near(c->handle, hw, &period_frames, 0);

    err = snd_pcm_hw_params(c->handle, hw);
    if (err < 0) {
        AOSL_LOG_ERR("init: hw_params failed: %s", snd_strerror(err));
        goto fail;
    }

    AOSL_LOG_DBG("init: hw_params ok (rate=%u, buf=%lu, period=%lu)",
                 actual_rate, (unsigned long)buf_frames, (unsigned long)period_frames);

    /* SW params */
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(c->handle, sw);
    snd_pcm_sw_params_set_avail_min(c->handle, sw, period_frames);
    snd_pcm_sw_params_set_start_threshold(c->handle, sw, 1);
    err = snd_pcm_sw_params(c->handle, sw);
    if (err < 0) {
        AOSL_LOG_ERR("init: sw_params failed: %s", snd_strerror(err));
        goto fail;
    }

    *ctx = c;
    AOSL_LOG_INF("init: ok");
    return 0;

fail:
    if (c->handle) { snd_pcm_close(c->handle); }
    free(c);
    return -1;
}

static int alsa_capture_start(void *ctx)
{
    (void)ctx;
    AOSL_LOG_INF("start");
    return 0;
}

static int alsa_capture_read(void *ctx, void *buf, int frames)
{
    alsa_cap_t *c = (alsa_cap_t *)ctx;
    int frame_bytes = c->bits_per_sample / 8 * c->channels;
    int want = frames * frame_bytes;
    int total_bytes = 0;
    int got;

    while (total_bytes < want) {
        /* Hand out any bytes buffered by a previous partial read first. */
        if (c->acc_len > 0) {
            int copy = (want - total_bytes) < c->acc_len
                           ? (want - total_bytes) : c->acc_len;
            memcpy((uint8_t *)buf + total_bytes, c->acc_buf, copy);
            total_bytes += copy;
            c->acc_len -= copy;
            if (c->acc_len > 0) {
                memmove(c->acc_buf, c->acc_buf + copy, c->acc_len);
            }
            if (total_bytes >= want) {
                break;
            }
        }

        got = pcm_read(c->handle, c->acc_buf, PCM_FRAMES_20MS);
        if (got < 0) {
            return -1;
        }
        if (got == 0) {
            return 0;   /* no new data within the wait window */
        }
        c->acc_len = got * frame_bytes;
    }

    return frames;
}

static int alsa_capture_stop(void *ctx)
{
    AOSL_LOG_INF("stop");
    if (ctx) {
        alsa_cap_t *c = (alsa_cap_t *)ctx;
        /* Immediate stop (does not wait for pending frames), safe to call
         * from another thread than the one inside snd_pcm_readi. */
        snd_pcm_drop(c->handle);
    }
    return 0;
}

static void alsa_capture_destroy(void *ctx)
{
    AOSL_LOG_INF("destroy");
    if (!ctx) {
        return;
    }
    alsa_cap_t *c = (alsa_cap_t *)ctx;
    if (c->handle) {
        snd_pcm_drop(c->handle);
        snd_pcm_close(c->handle);
    }
    free(c);
}

static const mybot_audio_capture_ops_t g_alsa_capture_ops = {
    .name    = "alsa",
    .init    = alsa_capture_init,
    .start   = alsa_capture_start,
    .read    = alsa_capture_read,
    .stop    = alsa_capture_stop,
    .destroy = alsa_capture_destroy,
};

void mybot_audio_platform_register_alsa_capture(void)
{
    mybot_audio_device_register_capture(&g_alsa_capture_ops);
    AOSL_LOG_INF("platform registered");
}
