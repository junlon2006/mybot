#include "audio/mybot_audio_device.h"

#include <alsa/asoundlib.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include <api/aosl_log.h>
#include <hal/aosl_hal_time.h>

/* Bounded wait for poll-based (non-blocking) PCM I/O. Keeps read/write
 * interruptible so worker threads can observe stop conditions and exit
 * promptly instead of blocking forever inside the driver. */
#define PCM_POLL_TIMEOUT_MS 50
#define PCM_RESUME_TIMEOUT_MS 500
#define PCM_RESUME_RETRY_MS 1

/* Internal context */
typedef struct {
    snd_pcm_t *handle;
    int rate;
    int channels;
    int bits_per_sample;

    /* Holds partial reads across calls for the frame size requested by the app. */
    uint8_t *acc_buf;
    size_t acc_capacity;
    size_t acc_len;
} alsa_cap_t;

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

static int pcm_read(snd_pcm_t *handle, char *buf, size_t frames, size_t frame_bytes) {
    ssize_t r;
    size_t count = frames;
    size_t result = 0;

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
            count -= (size_t)r;
            if (count > 0 && snd_pcm_wait(handle, PCM_POLL_TIMEOUT_MS) <= 0) {
                break; /* partial read; return what we have */
            }
        } else {
            break;
        }
    }
    return (int)result;
}

/* ---- capture ops implementation ---- */

static int alsa_capture_init(void **ctx, int rate, int channels, int bits) {
    if (!ctx) {
        return -1;
    }

    alsa_cap_t *c = (alsa_cap_t *)calloc(1, sizeof(alsa_cap_t));
    if (!c) {
        return -1;
    }

    c->rate = rate;
    c->channels = channels;
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
    err = snd_pcm_hw_params_set_rate_near(c->handle, hw, &actual_rate, 0);
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

    snd_pcm_uframes_t buf_frames = (snd_pcm_uframes_t)(rate * 50 / 1000);
    snd_pcm_hw_params_set_buffer_size_near(c->handle, hw, &buf_frames);

    snd_pcm_uframes_t period_frames = buf_frames / 4;
    snd_pcm_hw_params_set_period_size_near(c->handle, hw, &period_frames, 0);

    err = snd_pcm_hw_params(c->handle, hw);
    if (err < 0) {
        AOSL_LOG_ERR("init: hw_params failed: %s", snd_strerror(err));
        goto fail;
    }

    AOSL_LOG_DBG("init: hw_params ok (rate=%u, buf=%lu, period=%lu)", actual_rate,
                 (unsigned long)buf_frames, (unsigned long)period_frames);

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
    if (c->handle) {
        snd_pcm_close(c->handle);
    }
    free(c);
    return -1;
}

static int alsa_capture_start(void *ctx) {
    (void)ctx;
    AOSL_LOG_INF("start");
    return 0;
}

static int alsa_capture_read(void *ctx, void *buf, int frames) {
    alsa_cap_t *c = (alsa_cap_t *)ctx;
    if (!c || !buf || frames <= 0) {
        return -1;
    }

    size_t frame_bytes = (size_t)(c->bits_per_sample / 8 * c->channels);
    if (frame_bytes == 0 || (size_t)frames > SIZE_MAX / frame_bytes) {
        return -1;
    }
    size_t want = (size_t)frames * frame_bytes;
    if (want > c->acc_capacity) {
        uint8_t *new_buf = realloc(c->acc_buf, want);
        if (!new_buf) {
            return -1;
        }
        c->acc_buf = new_buf;
        c->acc_capacity = want;
    }

    int got;

    /* Accumulate until a full frame is available. Partial reads stay in
     * acc_buf across calls, so short reads never lose audio. */
    while (c->acc_len < want) {
        size_t remaining_frames = (want - c->acc_len) / frame_bytes;
        got = pcm_read(c->handle, (char *)c->acc_buf + c->acc_len, remaining_frames, frame_bytes);
        if (got < 0) {
            return -1;
        }
        if (got == 0) {
            return 0; /* no new data; keep whatever is already in acc_buf */
        }
        c->acc_len += (size_t)got * frame_bytes;
    }

    /* Hand out one full frame from the head of the accumulator. */
    memcpy((uint8_t *)buf, c->acc_buf, want);
    c->acc_len -= want;
    if (c->acc_len > 0) {
        memmove(c->acc_buf, c->acc_buf + want, c->acc_len);
    }
    return frames;
}

static int alsa_capture_stop(void *ctx) {
    AOSL_LOG_INF("stop");
    if (ctx) {
        alsa_cap_t *c = (alsa_cap_t *)ctx;
        /* Immediate stop (does not wait for pending frames), safe to call
         * from another thread than the one inside snd_pcm_readi. */
        snd_pcm_drop(c->handle);
    }
    return 0;
}

static void alsa_capture_destroy(void *ctx) {
    AOSL_LOG_INF("destroy");
    if (!ctx) {
        return;
    }
    alsa_cap_t *c = (alsa_cap_t *)ctx;
    if (c->handle) {
        snd_pcm_drop(c->handle);
        snd_pcm_close(c->handle);
    }
    free(c->acc_buf);
    free(c);
}

static const mybot_audio_capture_ops_t g_alsa_capture_ops = {
    .name = "alsa",
    .init = alsa_capture_init,
    .start = alsa_capture_start,
    .read = alsa_capture_read,
    .stop = alsa_capture_stop,
    .destroy = alsa_capture_destroy,
};

void linux_audio_platform_register_alsa_capture(void) {
    mybot_audio_device_register_capture(&g_alsa_capture_ops);
    AOSL_LOG_INF("platform registered");
}
