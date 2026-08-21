/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_announce_pcm_bk725x.h"

#include "bk_partition.h"
#include "mybot_assets.h"
#include "mybot_ogg_pcm_bk725x.h"

#include <common/bk_err.h>
#include "mybot_language.h"
#include "mybot_platform_log.h"
#include <os/mem.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define TAG "mybot_announce"

#define ANNOUNCE_RATE_HZ 16000
/* ~16 s of s16le mono, well beyond any pairing prompt. */
#define ANNOUNCE_MAX_FRAMES (512U * 1024U / 2U)
#define ANNOUNCE_PCM_ROOT                                                                    \
    MYBOT_ASSETS_DIR "/locales/" MYBOT_LANGUAGE_TAG

typedef struct {
    uint8_t reserved;
} announce_ctx_t;

typedef struct {
    int16_t *pcm;
    int frames;
    int offset;
} announce_sound_handle_t;

static const char *sound_file_name(mybot_announce_sound_t sound) {
    switch (sound) {
    case MYBOT_ANNOUNCE_SOUND_PROMPT:
        return "prompt.ogg";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_0:
        return "0.ogg";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_1:
        return "1.ogg";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_2:
        return "2.ogg";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_3:
        return "3.ogg";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_4:
        return "4.ogg";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_5:
        return "5.ogg";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_6:
        return "6.ogg";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_7:
        return "7.ogg";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_8:
        return "8.ogg";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_9:
        return "9.ogg";
    default:
        return NULL;
    }
}

static void release_sound(announce_sound_handle_t *sound) {
    if (!sound) {
        return;
    }
    if (sound->pcm) {
        psram_free(sound->pcm);
    }
    psram_free(sound);
}

int mybot_announce_pcm_bk725x_init(void **ctx) {
    announce_ctx_t *announce;

    if (!ctx) {
        return -1;
    }
    *ctx = NULL;

    announce = psram_zalloc(sizeof(*announce));
    if (!announce) {
        MYBOT_LOGE(TAG, "context allocation failed");
        return -1;
    }

    *ctx = announce;
    MYBOT_LOGI(TAG, "embedded Ogg/Opus source ready: %s (decoded to 16 kHz mono s16le)",
            ANNOUNCE_PCM_ROOT);
    return 0;
}

void *mybot_announce_pcm_bk725x_open(void *ctx, mybot_announce_sound_t sound) {
    announce_ctx_t *announce = ctx;
    announce_sound_handle_t *handle = NULL;
    mybot_asset_t asset;
    mybot_ogg_pcm_t ogg;
    const char *file_name = sound_file_name(sound);
    char path[VFS_FILE_MAX_LEN];
    int path_length;

    if (!announce || !file_name) {
        return NULL;
    }

    path_length = snprintf(path, sizeof(path), "%s/%s", ANNOUNCE_PCM_ROOT, file_name);
    if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
        MYBOT_LOGW(TAG, "ogg path is too long for %s", file_name);
        return NULL;
    }

    if (mybot_asset_find(path, &asset) < 0) {
        MYBOT_LOGW(TAG, "embedded ogg unavailable: %s", path);
        return NULL;
    }
    if (mybot_ogg_pcm_load_memory(path, asset.data, asset.size, ANNOUNCE_RATE_HZ, &ogg) < 0) {
        MYBOT_LOGW(TAG, "failed to decode embedded ogg: %s", path);
        return NULL;
    }
    if (ogg.frames <= 0 || (size_t)ogg.frames > ANNOUNCE_MAX_FRAMES) {
        MYBOT_LOGW(TAG, "decoded audio out of range: %s, frames=%d", path, ogg.frames);
        mybot_ogg_pcm_free(&ogg);
        return NULL;
    }

    handle = psram_zalloc(sizeof(*handle));
    if (!handle) {
        MYBOT_LOGE(TAG, "sound handle allocation failed: %s", path);
        mybot_ogg_pcm_free(&ogg);
        return NULL;
    }
    handle->pcm = ogg.pcm;
    handle->frames = ogg.frames;
    handle->offset = 0;
    MYBOT_LOGI(TAG, "ogg decoded: %s, frames=%d", path, ogg.frames);
    return handle;
}

int mybot_announce_pcm_bk725x_read(void *ctx, void *sound, int16_t *dst, int max_frames) {
    announce_sound_handle_t *handle = sound;
    int remaining;
    int frames;

    (void)ctx;
    if (!handle || !dst || max_frames <= 0) {
        return 0;
    }

    remaining = handle->frames - handle->offset;
    frames = remaining < max_frames ? remaining : max_frames;
    if (frames <= 0) {
        return 0;
    }

    memcpy(dst, handle->pcm + handle->offset, (size_t)frames * sizeof(int16_t));
    handle->offset += frames;
    return frames;
}

void mybot_announce_pcm_bk725x_close(void *ctx, void *sound) {
    (void)ctx;
    MYBOT_LOGI(TAG, "close requested");
    release_sound(sound);
}

void mybot_announce_pcm_bk725x_destroy(void *ctx) {
    announce_ctx_t *announce = ctx;
    MYBOT_LOGI(TAG, "destroy requested");

    if (!announce) {
        return;
    }
    psram_free(announce);
}
