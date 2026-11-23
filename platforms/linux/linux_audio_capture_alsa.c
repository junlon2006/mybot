/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_audio.h>

#include "linux_alsa_pcm.h"
#include "linux_platform_adapters.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include <api/aosl_log.h>

/* Internal context. */
typedef struct {
    snd_pcm_t *handle;
    int channels;
    int bits_per_sample;

    /* Holds partial reads across calls for the frame size requested by the app. */
    uint8_t *acc_buf;
    size_t acc_capacity;
    size_t acc_len;
} alsa_cap_t;

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
            if (linux_alsa_recover_xrun(handle, "overrun") < 0) {
                return -1;
            }
        } else if (r == -ESTRPIPE) {
            if (linux_alsa_recover_suspend(handle) < 0) {
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

/* ---- Capture ops implementation ---- */

static int alsa_capture_init(void **ctx, int rate, int channels, int bits) {
    if (!ctx) {
        return -1;
    }

    alsa_cap_t *c = (alsa_cap_t *)calloc(1, sizeof(alsa_cap_t));
    if (!c) {
        return -1;
    }

    c->channels = channels;
    c->bits_per_sample = bits;

    AOSL_LOG_NTC("init: rate=%d channels=%d bits=%d", rate, channels, bits);

    c->handle =
        linux_alsa_open_pcm(SND_PCM_STREAM_CAPTURE, rate, channels, 50, PCM_START_ON_FIRST_FRAME);
    if (!c->handle) {
        free(c);
        return -1;
    }
    *ctx = c;
    AOSL_LOG_NTC("init: ok");
    return 0;
}

static int alsa_capture_start(void *ctx) {
    (void)ctx;
    AOSL_LOG_NTC("start");
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
    AOSL_LOG_NTC("stop");
    if (ctx) {
        alsa_cap_t *c = (alsa_cap_t *)ctx;
        /* Immediate stop (does not wait for pending frames), safe to call
         * from another thread than the one inside snd_pcm_readi. */
        snd_pcm_drop(c->handle);
    }
    return 0;
}

static void alsa_capture_destroy(void *ctx) {
    AOSL_LOG_NTC("destroy");
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
    .init = alsa_capture_init,
    .start = alsa_capture_start,
    .read = alsa_capture_read,
    .stop = alsa_capture_stop,
    .destroy = alsa_capture_destroy,
};

const mybot_audio_capture_ops_t *linux_audio_platform_alsa_capture_ops(void) {
    return &g_alsa_capture_ops;
}
