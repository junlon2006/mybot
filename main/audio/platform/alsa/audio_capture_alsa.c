#include "audio_device.h"

#include <alsa/asoundlib.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define TAG "ALSA_CAP"

/* 16 kHz, 16-bit, mono → 20 ms = 640 bytes / frame */
#define PCM_FRAMES_20MS  320   /* 16000 * 20 / 1000 = 320 samples */
#define PCM_BYTES_20MS   640   /* 320 * 2 */

/* Internal context for one ALSA capture session */
typedef struct {
    snd_pcm_t      *handle;
    int             rate;
    int             channels;
    int             bits_per_sample;

    /* 20 ms frame accumulation buffer */
    uint8_t         acc_buf[PCM_BYTES_20MS];
    int             acc_len;          /* bytes accumulated so far */
} alsa_cap_t;

/* ---- ALSA error recovery helpers (from 3a-demo) ---- */
static int xrun_recover(snd_pcm_t *handle)
{
    snd_pcm_status_t *status;
    snd_pcm_status_alloca(&status);

    int err = snd_pcm_status(handle, status);
    if (err < 0) {
        fprintf(stderr, "[ALSA_CAP] status error: %s\n", snd_strerror(err));
        goto prepare;
    }
    if (snd_pcm_status_get_state(status) == SND_PCM_STATE_XRUN) {
        snd_pcm_uframes_t delay = snd_pcm_status_get_delay(status);
        fprintf(stderr, "[ALSA_CAP] overrun detected (%lu frames), recovering\n",
                (unsigned long)delay);
    }

prepare:
    err = snd_pcm_prepare(handle);
    if (err < 0) {
        fprintf(stderr, "[ALSA_CAP] prepare failed after xrun: %s\n", snd_strerror(err));
        return -1;
    }
    return 0;
}

static int suspend_recover(snd_pcm_t *handle)
{
    int err;
    while ((err = snd_pcm_resume(handle)) == -EAGAIN)
        usleep(1000);   /* wait for resume */
    if (err < 0) {
        err = snd_pcm_prepare(handle);
        if (err < 0) {
            fprintf(stderr, "[ALSA_CAP] resume/prepare failed: %s\n", snd_strerror(err));
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
    int     frame_bytes = 2; /* 16-bit mono = 2 bytes/frame */

    while (count > 0) {
        r = snd_pcm_readi(handle, buf + result * frame_bytes, count);
        if (r == -EAGAIN || (r >= 0 && (size_t)r < count)) {
            snd_pcm_wait(handle, 100);
        } else if (r == -EPIPE) {
            if (xrun_recover(handle) < 0)
                return -1;
        } else if (r == -ESTRPIPE) {
            if (suspend_recover(handle) < 0)
                return -1;
        } else if (r < 0) {
            fprintf(stderr, "[ALSA_CAP] read error: %s\n", snd_strerror(r));
            return -1;
        }
        if (r > 0) {
            result += r;
            count  -= r;
        }
    }
    return (int)result;
}

/* ---- capture ops implementation ---- */

static int alsa_capture_init(void **ctx, int rate, int channels, int bits)
{
    if (!ctx)
        return -1;

    alsa_cap_t *c = (alsa_cap_t *)calloc(1, sizeof(alsa_cap_t));
    if (!c)
        return -1;

    c->rate          = rate;
    c->channels      = channels;
    c->bits_per_sample = bits;

    int err;
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;

    /* Open device */
    err = snd_pcm_open(&c->handle, "default", SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        fprintf(stderr, "[ALSA_CAP] open failed: %s\n", snd_strerror(err));
        goto fail;
    }

    /* HW params */
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(c->handle, hw);

    snd_pcm_hw_params_set_access(c->handle, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(c->handle, hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(c->handle, hw, channels);

    unsigned int actual_rate = rate;
    snd_pcm_hw_params_set_rate_near(c->handle, hw, &actual_rate, 0);

    /* Buffer: up to 50 ms */
    snd_pcm_uframes_t buf_frames = (snd_pcm_uframes_t)(rate * 50 / 1000);
    snd_pcm_hw_params_set_buffer_size_near(c->handle, hw, &buf_frames);

    /* Period: 1/4 of buffer */
    snd_pcm_uframes_t period_frames = buf_frames / 4;
    snd_pcm_hw_params_set_period_size_near(c->handle, hw, &period_frames, 0);

    err = snd_pcm_hw_params(c->handle, hw);
    if (err < 0) {
        fprintf(stderr, "[ALSA_CAP] hw_params failed: %s\n", snd_strerror(err));
        goto fail;
    }

    /* SW params */
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(c->handle, sw);
    snd_pcm_sw_params_set_avail_min(c->handle, sw, period_frames);
    snd_pcm_sw_params_set_start_threshold(c->handle, sw, 1);
    err = snd_pcm_sw_params(c->handle, sw);
    if (err < 0) {
        fprintf(stderr, "[ALSA_CAP] sw_params failed: %s\n", snd_strerror(err));
        goto fail;
    }

    *ctx = c;
    return 0;

fail:
    if (c->handle) snd_pcm_close(c->handle);
    free(c);
    return -1;
}

static int alsa_capture_start(void *ctx)
{
    /* ALSA capture starts as soon as data is read — no explicit start needed */
    (void)ctx;
    return 0;
}

static int alsa_capture_read(void *ctx, void *buf, int frames)
{
    alsa_cap_t *c = (alsa_cap_t *)ctx;
    int total_bytes = 0;
    int want = frames * c->bits_per_sample / 8 * c->channels;
    int chunk_frames;

    while (total_bytes < want) {
        /* If accumulated buffer has data, drain it first */
        if (c->acc_len > 0) {
            int copy = (want - total_bytes) < c->acc_len
                           ? (want - total_bytes) : c->acc_len;
            memcpy((uint8_t *)buf + total_bytes, c->acc_buf, copy);
            total_bytes += copy;
            c->acc_len -= copy;
            if (c->acc_len > 0)
                memmove(c->acc_buf, c->acc_buf + copy, c->acc_len);
            if (total_bytes >= want)
                break;
        }

        /* Read one period from ALSA */
        chunk_frames = pcm_read(c->handle, (char *)c->acc_buf, PCM_FRAMES_20MS);
        if (chunk_frames <= 0) {
            /* On error, yield a bit and retry */
            usleep(5000);
            continue;
        }
        c->acc_len = chunk_frames * c->bits_per_sample / 8 * c->channels;
    }

    return frames;
}

static int alsa_capture_stop(void *ctx)
{
    if (ctx) {
        alsa_cap_t *c = (alsa_cap_t *)ctx;
        snd_pcm_drain(c->handle);
    }
    return 0;
}

static void alsa_capture_destroy(void *ctx)
{
    if (!ctx)
        return;
    alsa_cap_t *c = (alsa_cap_t *)ctx;
    if (c->handle) {
        snd_pcm_drain(c->handle);
        snd_pcm_close(c->handle);
    }
    free(c);
}

static const audio_capture_ops_t g_alsa_capture_ops = {
    .name    = "alsa",
    .init    = alsa_capture_init,
    .start   = alsa_capture_start,
    .read    = alsa_capture_read,
    .stop    = alsa_capture_stop,
    .destroy = alsa_capture_destroy,
};

/* Called once to register the ALSA capture backend */
void audio_platform_register_alsa_capture(void)
{
    audio_device_register_capture(&g_alsa_capture_ops);
}
