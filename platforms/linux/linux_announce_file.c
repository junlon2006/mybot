/* SPDX-License-Identifier: Apache-2.0 */
#include "linux_platform_adapters.h"

#include <mybot/platform/mybot_announce.h>

#include <api/aosl_log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Linux reference announcement adapter: raw 16 kHz mono signed 16-bit PCM
 * files, one file per logical sound, organized per locale:
 *
 *   <assets_dir>/locales/<locale>/prompt.pcm   (localized fixed pairing-code prompt)
 *   <assets_dir>/locales/<locale>/0.pcm ... 9.pcm
 *
 * Defaults: assets_dir = ./assets, locale = zh-CN. Both can be overridden
 * with the MYBOT_ASSETS_DIR and MYBOT_LOCALE environment variables.
 *
 * No decoder is used: the files must already be 16 kHz mono s16 PCM.
 */

#define MYBOT_ANNOUNCE_DEFAULT_DIR "assets"
#define MYBOT_ANNOUNCE_DEFAULT_LOCALE "zh-CN"
#define MYBOT_ANNOUNCE_PATH_CAPACITY 320

typedef struct {
    char base_dir[160];
    char locale[64];
} file_ctx_t;

typedef struct {
    int16_t *pcm;
    int frames;
    int offset;
} sound_handle_t;

static const char *sound_file_name(mybot_announce_sound_t sound) {
    switch (sound) {
    case MYBOT_ANNOUNCE_SOUND_PROMPT:
        return "prompt.pcm";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_0:
        return "0.pcm";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_1:
        return "1.pcm";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_2:
        return "2.pcm";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_3:
        return "3.pcm";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_4:
        return "4.pcm";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_5:
        return "5.pcm";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_6:
        return "6.pcm";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_7:
        return "7.pcm";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_8:
        return "8.pcm";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_9:
        return "9.pcm";
    default:
        return NULL;
    }
}

static int file_init(void **ctx) {
    file_ctx_t *c = (file_ctx_t *)calloc(1, sizeof(*c));
    if (!c) {
        return -1;
    }
    const char *dir = getenv("MYBOT_ASSETS_DIR");
    const char *locale = getenv("MYBOT_LOCALE");
    snprintf(c->base_dir, sizeof(c->base_dir), "%s",
             dir && dir[0] ? dir : MYBOT_ANNOUNCE_DEFAULT_DIR);
    snprintf(c->locale, sizeof(c->locale), "%s",
             locale && locale[0] ? locale : MYBOT_ANNOUNCE_DEFAULT_LOCALE);
    AOSL_LOG_NTC("announcement implementation: dir=%s locale=%s", c->base_dir, c->locale);
    *ctx = c;
    return 0;
}

static void *file_open(void *ctx, mybot_announce_sound_t sound) {
    file_ctx_t *c = (file_ctx_t *)ctx;
    const char *name = sound_file_name(sound);
    if (!c || !name) {
        return NULL;
    }

    char path[MYBOT_ANNOUNCE_PATH_CAPACITY];
    snprintf(path, sizeof(path), "%s/locales/%s/%s", c->base_dir, c->locale, name);

    FILE *f = fopen(path, "rb");
    if (!f) {
        AOSL_LOG_WRN("announce: cannot open %s", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size <= 0 || (size & 1L) != 0) {
        AOSL_LOG_WRN("announce: %s is not an even-sized PCM file (%ld bytes)", path, size);
        fclose(f);
        return NULL;
    }
    rewind(f);

    sound_handle_t *h = (sound_handle_t *)malloc(sizeof(*h));
    if (!h) {
        fclose(f);
        return NULL;
    }
    h->pcm = (int16_t *)malloc((size_t)size);
    if (!h->pcm) {
        free(h);
        fclose(f);
        return NULL;
    }
    size_t got = fread(h->pcm, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(h->pcm);
        free(h);
        AOSL_LOG_WRN("announce: short read on %s", path);
        return NULL;
    }

    h->frames = (int)(size / 2);
    h->offset = 0;
    return h;
}

static int file_read(void *ctx, void *sound, int16_t *dst, int max_frames) {
    (void)ctx;
    sound_handle_t *h = (sound_handle_t *)sound;
    if (!h) {
        return 0;
    }
    int remaining = h->frames - h->offset;
    int n = remaining < max_frames ? remaining : max_frames;
    if (n <= 0) {
        return 0;
    }
    memcpy(dst, h->pcm + h->offset, (size_t)n * sizeof(int16_t));
    h->offset += n;
    return n;
}

static void file_close(void *ctx, void *sound) {
    (void)ctx;
    sound_handle_t *h = (sound_handle_t *)sound;
    if (!h) {
        return;
    }
    free(h->pcm);
    free(h);
}

static void file_destroy(void *ctx) {
    free(ctx);
}

static const mybot_announce_ops_t s_file_ops = {
    .name = "file-pcm",
    .init = file_init,
    .open = file_open,
    .read = file_read,
    .close = file_close,
    .destroy = file_destroy,
};

int linux_announce_platform_register(void) {
    return mybot_announce_register(&s_file_ops);
}
