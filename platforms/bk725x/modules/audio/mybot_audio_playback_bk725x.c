/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_audio_playback_bk725x.h"

#include <components/bk_audio/audio_pipeline/audio_element.h>
#include <components/bk_audio/audio_pipeline/audio_pipeline.h>
#include <components/bk_audio/audio_streams/onboard_speaker_stream.h>
#include <components/bk_audio/audio_streams/raw_stream.h>
#include "mybot_platform_log.h"
#include <components/system.h>
#include <os/mem.h>
#include <os/os.h>

#include <limits.h>
#include <stdbool.h>

#include "FreeRTOS.h"

#define TAG "mybot_pb"

#define MYBOT_PLAYBACK_BYTES_PER_FRAME 2
#define MYBOT_PLAYBACK_ELEMENT_FRAME_MS 20
#define MYBOT_PLAYBACK_ELEMENT_FRAME_BYTES                                               \
    (MYBOT_PLAYBACK_RATE_HZ * MYBOT_PLAYBACK_ELEMENT_FRAME_MS / 1000 *                  \
     MYBOT_PLAYBACK_BYTES_PER_FRAME)
#define MYBOT_PLAYBACK_OUTPUT_BLOCKS 4
#define MYBOT_PLAYBACK_WRITE_TIMEOUT_MS 25

typedef struct {
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t raw_writer;
    audio_element_handle_t speaker;
    bool raw_writer_registered;
    bool speaker_registered;
    bool started;
    bool published;
} bk725x_playback_ctx_t;

static beken_mutex_t s_playback_lock;
static bk725x_playback_ctx_t *s_active_playback;

static int ensure_playback_lock(void) {
    if (s_playback_lock) {
        return 0;
    }
    if (rtos_init_mutex(&s_playback_lock) != BK_OK) {
        MYBOT_LOGE(TAG, "failed to initialize playback state lock");
        return -1;
    }
    return 0;
}

static int publish_playback(bk725x_playback_ctx_t *playback) {
    int result = -1;

    if (!playback || !playback->speaker || ensure_playback_lock() < 0 ||
        rtos_lock_mutex(&s_playback_lock) != BK_OK) {
        MYBOT_LOGE(TAG, "failed to lock active playback state");
        return -1;
    }

    /* The shared playback module may already hold a published pipeline.
     * Allow multiple publishers; the last one published is the active one
     * for digital-gain and is-active queries. */
    if (s_active_playback) {
        s_active_playback->published = false;
    }
    s_active_playback = playback;
    playback->published = true;
    result = 0;

    (void)rtos_unlock_mutex(&s_playback_lock);
    return result;
}

static bool unpublish_playback(bk725x_playback_ctx_t *playback) {
    if (!playback || !playback->published) {
        return true;
    }
    if (!s_playback_lock || rtos_lock_mutex(&s_playback_lock) != BK_OK) {
        MYBOT_LOGE(TAG, "failed to lock active playback state during release");
        return false;
    }

    if (s_active_playback == playback) {
        s_active_playback = NULL;
    }
    playback->published = false;
    (void)rtos_unlock_mutex(&s_playback_lock);
    return true;
}

bool mybot_audio_bk725x_playback_is_active(void) {
    bool active = false;

    if (!s_playback_lock || rtos_lock_mutex(&s_playback_lock) != BK_OK) {
        return false;
    }
    active = s_active_playback && s_active_playback->speaker;
    (void)rtos_unlock_mutex(&s_playback_lock);
    return active;
}

int mybot_audio_bk725x_playback_set_digital_gain(uint8_t gain) {
    int result = -1;

    if (!s_playback_lock || rtos_lock_mutex(&s_playback_lock) != BK_OK) {
        return -1;
    }
    if (s_active_playback && s_active_playback->speaker &&
        onboard_speaker_stream_set_digital_gain(s_active_playback->speaker, gain) == BK_OK) {
        result = 0;
    }
    (void)rtos_unlock_mutex(&s_playback_lock);
    return result;
}

int mybot_audio_bk725x_playback_get_digital_gain(uint8_t *gain) {
    int result = -1;

    if (!gain || !s_playback_lock || rtos_lock_mutex(&s_playback_lock) != BK_OK) {
        return -1;
    }
    if (s_active_playback && s_active_playback->speaker &&
        onboard_speaker_stream_get_digital_gain(s_active_playback->speaker, gain) == BK_OK) {
        result = 0;
    }
    (void)rtos_unlock_mutex(&s_playback_lock);
    return result;
}

static int stop_pipeline(bk725x_playback_ctx_t *playback) {
    int result = 0;

    if (!playback || !playback->started) {
        return 0;
    }

    if (audio_pipeline_stop(playback->pipeline) != BK_OK) {
        MYBOT_LOGE(TAG, "failed to stop playback pipeline");
        result = -1;
    }
    if (audio_pipeline_wait_for_stop(playback->pipeline) != BK_OK) {
        MYBOT_LOGE(TAG, "failed to wait for playback pipeline");
        result = -1;
    }

    playback->started = false;
    MYBOT_LOGI(TAG, "stop complete, result=%d", result);
    return result;
}

static bool release_playback(bk725x_playback_ctx_t *playback) {
    if (!playback) {
        return true;
    }

    /* Wait for an in-flight volume operation, then prevent any new access to
     * the speaker before the audio pipeline starts releasing its elements. */
    if (!unpublish_playback(playback)) {
        MYBOT_LOGE(TAG, "release aborted to preserve active speaker handle");
        return false;
    }

    (void)stop_pipeline(playback);

    if (playback->pipeline) {
        audio_pipeline_deinit(playback->pipeline);
        playback->pipeline = NULL;
    }

    if (playback->raw_writer && !playback->raw_writer_registered) {
        audio_element_deinit(playback->raw_writer);
    }
    if (playback->speaker && !playback->speaker_registered) {
        audio_element_deinit(playback->speaker);
    }

    psram_free(playback);
    return true;
}

int mybot_audio_bk725x_playback_init(void **ctx, int rate, int channels, int bits) {
    bk725x_playback_ctx_t *playback = NULL;
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    MYBOT_LOGI(TAG, "init requested: rate=%d ch=%d bits=%d", rate, channels, bits);
    onboard_speaker_stream_cfg_t speaker_cfg = ONBOARD_SPEAKER_STREAM_CFG_DEFAULT();
    const char *playback_link[] = {"raw_writer", "onboard_speaker"};

    if (!ctx) {
        MYBOT_LOGE(TAG, "init failed: invalid output context");
        return -1;
    }
    *ctx = NULL;

    if (rate != MYBOT_PLAYBACK_RATE_HZ || channels != MYBOT_PLAYBACK_CHANNELS ||
        bits != MYBOT_PLAYBACK_BITS) {
        MYBOT_LOGE(TAG, "unsupported format: %d Hz, %d channels, %d bits", rate, channels,
                bits);
        return -1;
    }
    if (ensure_playback_lock() < 0) {
        return -1;
    }

    playback = psram_zalloc(sizeof(*playback));
    if (!playback) {
        MYBOT_LOGE(TAG, "context allocation failed");
        return -1;
    }

    playback->pipeline = audio_pipeline_init(&pipeline_cfg);
    if (!playback->pipeline) {
        MYBOT_LOGE(TAG, "pipeline init failed");
        goto fail;
    }

    raw_cfg.type = AUDIO_STREAM_WRITER;
    raw_cfg.out_block_size = MYBOT_PLAYBACK_ELEMENT_FRAME_BYTES;
    raw_cfg.out_block_num = MYBOT_PLAYBACK_OUTPUT_BLOCKS;
    raw_cfg.output_port_type = PORT_TYPE_RB;
    playback->raw_writer = raw_stream_init(&raw_cfg);
    if (!playback->raw_writer) {
        MYBOT_LOGE(TAG, "raw writer init failed");
        goto fail;
    }
    if (audio_element_set_output_timeout(
            playback->raw_writer, BK_MS_TO_TICKS(MYBOT_PLAYBACK_WRITE_TIMEOUT_MS)) != BK_OK) {
        MYBOT_LOGE(TAG, "output timeout setup failed");
        goto fail;
    }

    speaker_cfg.chl_num = MYBOT_PLAYBACK_CHANNELS;
    speaker_cfg.sample_rate = MYBOT_PLAYBACK_RATE_HZ;
    speaker_cfg.bits = MYBOT_PLAYBACK_BITS;
    speaker_cfg.frame_size = MYBOT_PLAYBACK_ELEMENT_FRAME_BYTES;
    speaker_cfg.dig_gain = CONFIG_AE_DEFAULT_DIG_GAIN;
    speaker_cfg.ana_gain = CONFIG_AE_DEFAULT_ANA_GAIN;
    speaker_cfg.pa_ctrl_en = CONFIG_AE_ENABLE_PA_CNTRL;
    speaker_cfg.pa_ctrl_gpio = CONFIG_AE_PA_CNTRL_GPIO;
    speaker_cfg.pa_on_level = CONFIG_AE_PA_ON_LEVEL;
    speaker_cfg.pa_on_delay = CONFIG_AE_PA_ON_DELAY;
    speaker_cfg.pa_off_delay = CONFIG_AE_PA_OFF_DELAY;
    playback->speaker = onboard_speaker_stream_init(&speaker_cfg);
    if (!playback->speaker) {
        MYBOT_LOGE(TAG, "speaker init failed");
        goto fail;
    }

    if (audio_pipeline_register(playback->pipeline, playback->raw_writer, playback_link[0]) !=
        BK_OK) {
        MYBOT_LOGE(TAG, "raw writer register failed");
        goto fail;
    }
    playback->raw_writer_registered = true;

    if (audio_pipeline_register(playback->pipeline, playback->speaker, playback_link[1]) !=
        BK_OK) {
        MYBOT_LOGE(TAG, "speaker register failed");
        goto fail;
    }
    playback->speaker_registered = true;

    if (audio_pipeline_link(playback->pipeline, playback_link, 2) != BK_OK) {
        MYBOT_LOGE(TAG, "pipeline link failed");
        goto fail;
    }

    if (publish_playback(playback) < 0) {
        goto fail;
    }

    *ctx = playback;
    MYBOT_LOGI(TAG, "init success: %d Hz, %d channels, %d bits", rate, channels, bits);
    return 0;

fail:
    MYBOT_LOGE(TAG, "failed to initialize playback pipeline");
    release_playback(playback);
    return -1;
}

int mybot_audio_bk725x_playback_start(void *ctx) {
    bk725x_playback_ctx_t *playback = ctx;
    MYBOT_LOGI(TAG, "start requested");

    if (!playback || !playback->pipeline) {
        return -1;
    }
    if (playback->started) {
        return 0;
    }
    if (audio_pipeline_run(playback->pipeline) != BK_OK) {
        MYBOT_LOGE(TAG, "failed to start playback pipeline");
        return -1;
    }

    playback->started = true;
    MYBOT_LOGI(TAG, "start success");
    return 0;
}

int mybot_audio_bk725x_playback_write(void *ctx, const void *buf, int frames) {
    bk725x_playback_ctx_t *playback = ctx;
    int bytes_requested;
    int bytes_written;
    if (!playback) {
        MYBOT_LOGE(TAG, "write failed: playback context is NULL");
        return -1;
    }
    if (!playback->started) {
        MYBOT_LOGE(TAG, "write failed: playback not started");
        return -1;
    }
    if (!playback->raw_writer) {
        MYBOT_LOGE(TAG, "write failed: raw_writer is NULL");
        return -1;
    }
    if (!buf) {
        MYBOT_LOGE(TAG, "write failed: buf is NULL");
        return -1;
    }
    if (frames <= 0) {
        MYBOT_LOGE(TAG, "write failed: frames=%d <= 0", frames);
        return -1;
    }
    if (frames > INT_MAX / MYBOT_PLAYBACK_BYTES_PER_FRAME) {
        MYBOT_LOGE(TAG, "write failed: frames=%d too large", frames);
        return -1;
    }

    bytes_requested = frames * MYBOT_PLAYBACK_BYTES_PER_FRAME;
    bytes_written = raw_stream_write(playback->raw_writer, (char *)buf, bytes_requested);
    if (bytes_written == 0 || bytes_written == AEL_IO_TIMEOUT) {
        return 0;
    }
    if (bytes_written < 0 || bytes_written > bytes_requested ||
        bytes_written % MYBOT_PLAYBACK_BYTES_PER_FRAME != 0) {
        MYBOT_LOGE(TAG, "invalid playback write result: %d/%d bytes", bytes_written,
                bytes_requested);
        return -1;
    }

    return bytes_written / MYBOT_PLAYBACK_BYTES_PER_FRAME;
}

int mybot_audio_bk725x_playback_stop(void *ctx) {
    MYBOT_LOGI(TAG, "stop requested");
    return stop_pipeline(ctx);
}

void mybot_audio_bk725x_playback_destroy(void *ctx) {
    MYBOT_LOGI(TAG, "destroy requested");
    if (release_playback(ctx)) {
        MYBOT_LOGI(TAG, "destroy complete");
    }
}
