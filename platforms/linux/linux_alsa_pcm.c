/* SPDX-License-Identifier: Apache-2.0 */
#include "linux_alsa_pcm.h"

#include <api/aosl_log.h>
#include <hal/aosl_hal_time.h>

#define PCM_RESUME_TIMEOUT_MS 500
#define PCM_RESUME_RETRY_MS 1

int linux_alsa_recover_xrun(snd_pcm_t *handle, const char *kind) {
    snd_pcm_status_t *status;
    snd_pcm_status_alloca(&status);

    int err = snd_pcm_status(handle, status);
    if (err < 0) {
        AOSL_LOG_ERR("xrun_recover: status error: %s", snd_strerror(err));
        goto prepare;
    }
    if (snd_pcm_status_get_state(status) == SND_PCM_STATE_XRUN) {
        snd_pcm_uframes_t delay = snd_pcm_status_get_delay(status);
        AOSL_LOG_WRN("xrun: %s detected (%lu frames), recovering", kind, (unsigned long)delay);
    }

prepare:
    err = snd_pcm_prepare(handle);
    if (err < 0) {
        AOSL_LOG_ERR("xrun_recover: prepare failed: %s", snd_strerror(err));
        return -1;
    }
    return 0;
}

int linux_alsa_recover_suspend(snd_pcm_t *handle) {
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

snd_pcm_t *linux_alsa_open_pcm(snd_pcm_stream_t stream, int rate, int channels, int buffer_ms,
                               linux_alsa_start_policy_t start) {
    int err;
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;

    snd_pcm_t *handle = NULL;
    err = snd_pcm_open(&handle, "default", stream, 0);
    if (err < 0) {
        AOSL_LOG_ERR("init: snd_pcm_open failed: %s", snd_strerror(err));
        goto fail;
    }

    /* The I/O workers use bounded waits so shutdown can interrupt them. */
    snd_pcm_nonblock(handle, 1);

    /* Hardware parameters. */
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(handle, hw);
    snd_pcm_hw_params_set_access(handle, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(handle, hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(handle, hw, channels);

    unsigned int actual_rate = rate;
    err = snd_pcm_hw_params_set_rate_near(handle, hw, &actual_rate, 0);
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

    snd_pcm_uframes_t buf_frames = (snd_pcm_uframes_t)(rate * buffer_ms / 1000);
    snd_pcm_hw_params_set_buffer_size_near(handle, hw, &buf_frames);

    snd_pcm_uframes_t period_frames = buf_frames / 4;
    snd_pcm_hw_params_set_period_size_near(handle, hw, &period_frames, 0);

    err = snd_pcm_hw_params(handle, hw);
    if (err < 0) {
        AOSL_LOG_ERR("init: hw_params failed: %s", snd_strerror(err));
        goto fail;
    }

    AOSL_LOG_DBG("init: hw_params ok (rate=%u, buf=%lu, period=%lu)", actual_rate,
                 (unsigned long)buf_frames, (unsigned long)period_frames);

    /* Software parameters. */
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(handle, sw);
    snd_pcm_sw_params_set_avail_min(handle, sw, period_frames);
    snd_pcm_sw_params_set_start_threshold(handle, sw,
                                          start == PCM_START_ON_FULL_BUFFER ? buf_frames : 1);
    err = snd_pcm_sw_params(handle, sw);
    if (err < 0) {
        AOSL_LOG_ERR("init: sw_params failed: %s", snd_strerror(err));
        goto fail;
    }

    return handle;

fail:
    if (handle) {
        snd_pcm_close(handle);
    }
    return NULL;
}
