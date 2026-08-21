/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_audio_capture_bk725x.h"

#include <components/bk_audio/audio_pipeline/audio_element.h>
#include <components/bk_audio/audio_pipeline/audio_pipeline.h>
#include <components/bk_audio/audio_streams/onboard_mic_stream.h>
#include <components/bk_audio/audio_streams/raw_stream.h>
#include "mybot_platform_log.h"
#include <components/system.h>
#include <os/mem.h>

#include <limits.h>
#include <stdbool.h>

#include "FreeRTOS.h"

#define TAG "mybot_cap"

#define MYBOT_CAPTURE_RATE_HZ 16000
#define MYBOT_CAPTURE_CHANNELS 1
#define MYBOT_CAPTURE_BITS 16
#define MYBOT_CAPTURE_BYTES_PER_FRAME 2
#define MYBOT_CAPTURE_ELEMENT_FRAME_MS 20
#define MYBOT_CAPTURE_ELEMENT_FRAME_BYTES                                                   \
    (MYBOT_CAPTURE_RATE_HZ * MYBOT_CAPTURE_ELEMENT_FRAME_MS / 1000 *                        \
     MYBOT_CAPTURE_BYTES_PER_FRAME)
#define MYBOT_CAPTURE_OUTPUT_BLOCKS 4
#define MYBOT_CAPTURE_READ_TIMEOUT_MS 25
#define MYBOT_CAPTURE_DIGITAL_GAIN 0x30
#define MYBOT_CAPTURE_ANALOG_GAIN 0x08

typedef struct {
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t mic;
    audio_element_handle_t raw_reader;
    bool mic_registered;
    bool raw_reader_registered;
    bool started;
} bk725x_capture_ctx_t;

static int stop_pipeline(bk725x_capture_ctx_t *capture) {
    int result = 0;

    if (!capture || !capture->started) {
        return 0;
    }

    if (audio_pipeline_stop(capture->pipeline) != BK_OK) {
        MYBOT_LOGE(TAG, "failed to stop capture pipeline");
        result = -1;
    }
    if (audio_pipeline_wait_for_stop(capture->pipeline) != BK_OK) {
        MYBOT_LOGE(TAG, "failed to wait for capture pipeline");
        result = -1;
    }

    capture->started = false;
    MYBOT_LOGI(TAG, "stop complete, result=%d", result);
    return result;
}

static void release_capture(bk725x_capture_ctx_t *capture) {
    if (!capture) {
        return;
    }

    (void)stop_pipeline(capture);

    if (capture->pipeline) {
        audio_pipeline_deinit(capture->pipeline);
        capture->pipeline = NULL;
    }

    if (capture->mic && !capture->mic_registered) {
        audio_element_deinit(capture->mic);
    }
    if (capture->raw_reader && !capture->raw_reader_registered) {
        audio_element_deinit(capture->raw_reader);
    }

    psram_free(capture);
}

int mybot_audio_bk725x_capture_init(void **ctx, int rate, int channels, int bits) {
    bk725x_capture_ctx_t *capture = NULL;
    MYBOT_LOGI(TAG, "init requested: rate=%d ch=%d bits=%d", rate, channels, bits);
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    onboard_mic_stream_cfg_t mic_cfg = ONBOARD_MIC_ADC_STREAM_CFG_DEFAULT();
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    const char *capture_link[] = {"onboard_mic", "raw_reader"};

    if (!ctx) {
        MYBOT_LOGE(TAG, "init failed: invalid output context");
        return -1;
    }
    *ctx = NULL;

    if (rate != MYBOT_CAPTURE_RATE_HZ || channels != MYBOT_CAPTURE_CHANNELS ||
        bits != MYBOT_CAPTURE_BITS) {
        MYBOT_LOGE(TAG, "unsupported format: %d Hz, %d channels, %d bits", rate, channels,
                bits);
        return -1;
    }

    capture = psram_zalloc(sizeof(*capture));
    if (!capture) {
        MYBOT_LOGE(TAG, "context allocation failed");
        return -1;
    }

    capture->pipeline = audio_pipeline_init(&pipeline_cfg);
    if (!capture->pipeline) {
        MYBOT_LOGE(TAG, "pipeline init failed");
        goto fail;
    }

    mic_cfg.adc_cfg.sample_rate = MYBOT_CAPTURE_RATE_HZ;
    mic_cfg.adc_cfg.chl_num = MYBOT_CAPTURE_CHANNELS;
    mic_cfg.adc_cfg.bits = MYBOT_CAPTURE_BITS;
    mic_cfg.adc_cfg.dig_gain = MYBOT_CAPTURE_DIGITAL_GAIN;
    mic_cfg.adc_cfg.ana_gain = MYBOT_CAPTURE_ANALOG_GAIN;
    mic_cfg.frame_size = MYBOT_CAPTURE_ELEMENT_FRAME_BYTES;
    mic_cfg.out_block_size = MYBOT_CAPTURE_ELEMENT_FRAME_BYTES;
    mic_cfg.out_block_num = MYBOT_CAPTURE_OUTPUT_BLOCKS;
    capture->mic = onboard_mic_stream_init(&mic_cfg);
    if (!capture->mic) {
        MYBOT_LOGE(TAG, "microphone init failed");
        goto fail;
    }

    raw_cfg.type = AUDIO_STREAM_READER;
    capture->raw_reader = raw_stream_init(&raw_cfg);
    if (!capture->raw_reader) {
        MYBOT_LOGE(TAG, "raw reader init failed");
        goto fail;
    }
    if (audio_element_set_input_timeout(capture->raw_reader,
                                        BK_MS_TO_TICKS(MYBOT_CAPTURE_READ_TIMEOUT_MS)) != BK_OK) {
        MYBOT_LOGE(TAG, "input timeout setup failed");
        goto fail;
    }

    if (audio_pipeline_register(capture->pipeline, capture->mic, capture_link[0]) != BK_OK) {
        MYBOT_LOGE(TAG, "microphone register failed");
        goto fail;
    }
    capture->mic_registered = true;

    if (audio_pipeline_register(capture->pipeline, capture->raw_reader, capture_link[1]) !=
        BK_OK) {
        MYBOT_LOGE(TAG, "raw reader register failed");
        goto fail;
    }
    capture->raw_reader_registered = true;

    if (audio_pipeline_link(capture->pipeline, capture_link, 2) != BK_OK) {
        MYBOT_LOGE(TAG, "pipeline link failed");
        goto fail;
    }

    *ctx = capture;
    MYBOT_LOGI(TAG, "init success: %d Hz, %d channels, %d bits", rate, channels, bits);
    return 0;

fail:
    MYBOT_LOGE(TAG, "failed to initialize capture pipeline");
    release_capture(capture);
    return -1;
}

int mybot_audio_bk725x_capture_start(void *ctx) {
    bk725x_capture_ctx_t *capture = ctx;
    MYBOT_LOGI(TAG, "start requested");

    if (!capture || !capture->pipeline) {
        return -1;
    }
    if (capture->started) {
        return 0;
    }
    if (audio_pipeline_run(capture->pipeline) != BK_OK) {
        MYBOT_LOGE(TAG, "failed to start capture pipeline");
        return -1;
    }

    capture->started = true;
    MYBOT_LOGI(TAG, "start success");
    return 0;
}

int mybot_audio_bk725x_capture_read(void *ctx, void *buf, int frames) {
    bk725x_capture_ctx_t *capture = ctx;
    int bytes_requested;
    int bytes_read;

    if (!capture) {
        MYBOT_LOGE(TAG, "read failed: capture context is NULL");
        return -1;
    }
    if (!capture->started) {
        MYBOT_LOGE(TAG, "read failed: capture not started");
        return -1;
    }
    if (!capture->raw_reader) {
        MYBOT_LOGE(TAG, "read failed: raw_reader is NULL");
        return -1;
    }
    if (!buf) {
        MYBOT_LOGE(TAG, "read failed: buf is NULL");
        return -1;
    }
    if (frames <= 0) {
        MYBOT_LOGE(TAG, "read failed: frames=%d <= 0", frames);
        return -1;
    }
    if (frames > INT_MAX / MYBOT_CAPTURE_BYTES_PER_FRAME) {
        MYBOT_LOGE(TAG, "read failed: frames=%d too large", frames);
        return -1;
    }

    bytes_requested = frames * MYBOT_CAPTURE_BYTES_PER_FRAME;
    bytes_read = raw_stream_read(capture->raw_reader, buf, bytes_requested);
    if (bytes_read == 0 || bytes_read == AEL_IO_TIMEOUT) {
        return 0;
    }
    if (bytes_read < 0 || bytes_read > bytes_requested ||
        bytes_read % MYBOT_CAPTURE_BYTES_PER_FRAME != 0) {
        MYBOT_LOGE(TAG, "invalid capture read result: %d/%d bytes", bytes_read,
                bytes_requested);
        return -1;
    }

    return bytes_read / MYBOT_CAPTURE_BYTES_PER_FRAME;
}

int mybot_audio_bk725x_capture_stop(void *ctx) {
    MYBOT_LOGI(TAG, "stop requested");
    return stop_pipeline(ctx);
}


void mybot_audio_bk725x_capture_destroy(void *ctx) {
    MYBOT_LOGI(TAG, "destroy requested");
    release_capture(ctx);
    MYBOT_LOGI(TAG, "destroy complete");
}
