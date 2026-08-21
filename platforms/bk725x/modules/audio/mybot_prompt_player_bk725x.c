/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_prompt_player_bk725x.h"

#include "mybot_audio_shared_bk725x.h"
#include "mybot_assets.h"
#include "mybot_ogg_pcm_bk725x.h"

#include <common/bk_err.h>
#include "mybot_language.h"
#include "mybot_platform_log.h"
#include <os/mem.h>
#include <os/os.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TAG "mybot_prompt"

#define PROMPT_THREAD_PRIORITY 2
#define PROMPT_THREAD_STACK_SIZE 8192
/* ~16 s of s16le mono, far beyond any provisioning prompt. */
#define PROMPT_MAX_FRAMES (512U * 1024U / 2U)
#define PROMPT_RATE_HZ 16000
#define PROMPT_CHANNELS 1
#define PROMPT_BITS 16
#define PROMPT_FRAMES_PER_WRITE 320
#define PROMPT_DRAIN_MS 200
#define PROMPT_MAX_CONSECUTIVE_TIMEOUTS 100
#define PROVISIONING_PROMPT_PATH                                                         \
    MYBOT_ASSETS_DIR "/locales/" MYBOT_LANGUAGE_TAG "/wificonfig.ogg"
#define SUCCESS_PROMPT_PATH                                                             \
    MYBOT_ASSETS_DIR "/locales/" MYBOT_LANGUAGE_TAG "/success.ogg"

static beken_thread_t s_prompt_thread;
static beken_semaphore_t s_stop_requested;
static const char *s_prompt_path;

static bool stop_requested(uint32_t timeout_ms) {
    if (rtos_get_semaphore(&s_stop_requested, timeout_ms) != BK_OK) {
        return false;
    }
    /* Keep cancellation sticky across later checks in the worker. */
    rtos_set_semaphore(&s_stop_requested);
    return true;
}

static int load_pcm(int16_t **pcm, int *frames) {
    mybot_asset_t asset;
    mybot_ogg_pcm_t ogg;

    *pcm = NULL;
    *frames = 0;

    if (!s_prompt_path) {
        MYBOT_LOGE(TAG, "no prompt path configured");
        return -1;
    }

    if (mybot_asset_find(s_prompt_path, &asset) < 0) {
        MYBOT_LOGW(TAG, "embedded prompt is unavailable: %s", s_prompt_path);
        return -1;
    }

    if (mybot_ogg_pcm_load_memory(s_prompt_path, asset.data, asset.size, PROMPT_RATE_HZ,
                                  &ogg) < 0) {
        MYBOT_LOGW(TAG, "failed to decode embedded prompt: %s", s_prompt_path);
        return -1;
    }
    if (ogg.frames <= 0 || (size_t)ogg.frames > PROMPT_MAX_FRAMES) {
        MYBOT_LOGW(TAG, "decoded prompt out of range: frames=%d", ogg.frames);
        mybot_ogg_pcm_free(&ogg);
        return -1;
    }

    *pcm = ogg.pcm;
    *frames = ogg.frames;
    MYBOT_LOGI(TAG, "embedded prompt decoded: %s, frames=%d", s_prompt_path, ogg.frames);
    return 0;
}

static int play_pcm(const int16_t *pcm, int frames) {
    int offset = 0;
    int consecutive_timeouts = 0;
    int result = -1;

    /* Use the shared playback pipeline so the prompt and the SDK share
     * a single speaker path.  The pipeline, ring buffer, power vote and
     * volume are all managed by the shared module. */
    if (mybot_audio_bk725x_shared_playback_start() < 0) {
        MYBOT_LOGE(TAG, "shared playback start failed");
        return -1;
    }

    while (offset < frames && !stop_requested(BEKEN_NO_WAIT)) {
        int requested = frames - offset;
        if (requested > PROMPT_FRAMES_PER_WRITE) {
            requested = PROMPT_FRAMES_PER_WRITE;
        }
        int written = mybot_audio_bk725x_shared_playback_write(pcm + offset, requested);
        if (written < 0) {
            MYBOT_LOGE(TAG, "prompt write failed at frame %d/%d", offset, frames);
            goto cleanup;
        }
        if (written == 0) {
            if (++consecutive_timeouts >= PROMPT_MAX_CONSECUTIVE_TIMEOUTS) {
                MYBOT_LOGE(TAG, "prompt write timed out repeatedly");
                goto cleanup;
            }
            continue;
        }
        consecutive_timeouts = 0;
        offset += written;
    }

    if (offset == frames) {
        /* Let the raw-stream and speaker buffers drain; a stop request wakes this early. */
        (void)stop_requested(PROMPT_DRAIN_MS);
        result = 0;
    } else {
        MYBOT_LOGI(TAG, "prompt playback cancelled");
        result = 0;
    }

cleanup:
    /* Leave the shared pipeline running — the SDK will use it later. */
    return result;
}

static void prompt_thread_main(beken_thread_arg_t arg) {
    int16_t *pcm = NULL;
    int frames = 0;

    (void)arg;
    MYBOT_LOGI(TAG, "prompt worker started: %s", s_prompt_path);
    if (!stop_requested(BEKEN_NO_WAIT) && load_pcm(&pcm, &frames) == 0 &&
        !stop_requested(BEKEN_NO_WAIT)) {
        if (play_pcm(pcm, frames) < 0) {
            MYBOT_LOGW(TAG, "prompt playback failed");
        }
    }
    if (pcm) {
        psram_free(pcm);
    }
    MYBOT_LOGI(TAG, "prompt worker stopped");
    rtos_delete_thread(NULL);
}

static int start_prompt_thread(const char *path) {
    mybot_prompt_player_bk725x_stop();

    s_prompt_path = path;

    if (rtos_init_semaphore(&s_stop_requested, 1) != BK_OK) {
        MYBOT_LOGE(TAG, "failed to initialize stop signal");
        return -1;
    }
    if (rtos_create_psram_thread(&s_prompt_thread, PROMPT_THREAD_PRIORITY, "mybot_prompt",
                                 prompt_thread_main, PROMPT_THREAD_STACK_SIZE, NULL) != BK_OK) {
        s_prompt_thread = NULL;
        rtos_deinit_semaphore(&s_stop_requested);
        MYBOT_LOGE(TAG, "failed to create prompt worker");
        return -1;
    }
    return 0;
}

int mybot_prompt_player_bk725x_play_provisioning(void) {
    MYBOT_LOGI(TAG, "play provisioning prompt requested");
    return start_prompt_thread(PROVISIONING_PROMPT_PATH);
}

int mybot_prompt_player_bk725x_play_success(void) {
    MYBOT_LOGI(TAG, "play success prompt requested");
    return start_prompt_thread(SUCCESS_PROMPT_PATH);
}

void mybot_prompt_player_bk725x_stop(void) {
    MYBOT_LOGI(TAG, "stop prompt player requested");
    if (!s_prompt_thread) {
        return;
    }

    rtos_set_semaphore(&s_stop_requested);
    rtos_thread_join(&s_prompt_thread);
    s_prompt_thread = NULL;
    rtos_deinit_semaphore(&s_stop_requested);
}
