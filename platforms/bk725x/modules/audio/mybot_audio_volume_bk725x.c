/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_audio_volume_bk725x.h"
#include "mybot_audio_playback_bk725x.h"
#include "mybot_audio_internal_bk725x.h"

#include <bk_ef.h>
#include "mybot_platform_log.h"
#include <os/mem.h>
#include <os/os.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TAG "mybot_vol"

#define MYBOT_VOLUME_CONTEXT_MAGIC 0x564f4c55u
#define MYBOT_VOLUME_RECORD_KEY "mybot_volume_v1"
#define MYBOT_VOLUME_RECORD_MAGIC 0x4d42564cu
#define MYBOT_VOLUME_RECORD_VERSION 1u
#define MYBOT_VOLUME_LEVEL_STEP 10
#define MYBOT_VOLUME_LEVEL_COUNT 11
#define MYBOT_VOLUME_MIN 0
#define MYBOT_VOLUME_MAX 100
#define MYBOT_VOLUME_DEFAULT_LEVEL 8
#define MYBOT_DIGITAL_GAIN_MAX 0x3f
#define MYBOT_VOLUME_CURVE_DEFAULT_PERCENT 71
#define MYBOT_VOLUME_FULL_SCALE_GAIN                                                           \
    ((CONFIG_AE_DEFAULT_DIG_GAIN * 100 + MYBOT_VOLUME_CURVE_DEFAULT_PERCENT - 1) /              \
     MYBOT_VOLUME_CURVE_DEFAULT_PERCENT)

typedef struct {
    uint32_t magic;
    int volume;
} bk725x_volume_ctx_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    int32_t volume;
} bk725x_volume_record_t;

_Static_assert(sizeof(bk725x_volume_record_t) == 12, "volume record size");
_Static_assert(CONFIG_AE_DEFAULT_DIG_GAIN > 0, "default digital gain must be audible");
_Static_assert(MYBOT_VOLUME_FULL_SCALE_GAIN <= MYBOT_DIGITAL_GAIN_MAX,
               "default digital gain leaves no headroom for volume levels above 80");

/* Percentage curve used by the Beken audio engine for logical levels 0..100. */
static const uint8_t s_volume_curve_percent[MYBOT_VOLUME_LEVEL_COUNT] = {
    0, 6, 12, 20, 28, 37, 47, 58, 71, 84, 100,
};

static beken_mutex_t s_volume_lock;
static bool s_volume_known;
static int s_last_volume;
static bool s_persisted_volume_known;
static int s_persisted_volume;
static bool s_volume_persist_dirty;

static int ensure_volume_lock(void) {
    if (s_volume_lock) {
        return 0;
    }
    if (rtos_init_mutex(&s_volume_lock) != BK_OK) {
        MYBOT_LOGE(TAG, "volume lock initialization failed");
        return -1;
    }
    return 0;
}

static uint8_t volume_level_to_gain(int level) {
    if (level <= 0) {
        return 0;
    }
    if (level >= MYBOT_VOLUME_LEVEL_COUNT) {
        level = MYBOT_VOLUME_LEVEL_COUNT - 1;
    }
    if (level == MYBOT_VOLUME_DEFAULT_LEVEL) {
        return CONFIG_AE_DEFAULT_DIG_GAIN;
    }
    return (uint8_t)(MYBOT_VOLUME_FULL_SCALE_GAIN * s_volume_curve_percent[level] / 100);
}

static uint8_t volume_to_gain(int volume) {
    int level = volume / MYBOT_VOLUME_LEVEL_STEP;
    int remainder = volume % MYBOT_VOLUME_LEVEL_STEP;

    if (level >= MYBOT_VOLUME_LEVEL_COUNT - 1) {
        return volume_level_to_gain(MYBOT_VOLUME_LEVEL_COUNT - 1);
    }

    int lower = volume_level_to_gain(level);
    int upper = volume_level_to_gain(level + 1);
    int gain = lower + ((upper - lower) * remainder + MYBOT_VOLUME_LEVEL_STEP / 2) /
                           MYBOT_VOLUME_LEVEL_STEP;

    /* Volume zero alone is mute; preserve audible output for any non-zero value. */
    if (volume > MYBOT_VOLUME_MIN && gain == 0) {
        gain = 1;
    }
    return (uint8_t)gain;
}

static int gain_to_volume(uint8_t gain) {
    int closest_level = 0;
    int closest_distance = INT_MAX;

    for (int level = 0; level < MYBOT_VOLUME_LEVEL_COUNT; ++level) {
        uint8_t level_gain = volume_level_to_gain(level);
        int distance = gain > level_gain ? gain - level_gain : level_gain - gain;
        if (distance < closest_distance) {
            closest_distance = distance;
            closest_level = level;
        }
    }
    return closest_level * MYBOT_VOLUME_LEVEL_STEP;
}

static bool volume_context_is_valid(const bk725x_volume_ctx_t *volume) {
    return volume && volume->magic == MYBOT_VOLUME_CONTEXT_MAGIC;
}

static bool load_volume(int *stored_volume) {
    uint8_t raw[sizeof(bk725x_volume_record_t) + 1] = {0};
    bk725x_volume_record_t record;

    if (!stored_volume) {
        return false;
    }

    int length = bk_get_env_enhance(MYBOT_VOLUME_RECORD_KEY, raw, sizeof(raw));
    if (length == 0) {
        MYBOT_LOGI(TAG, "no persisted volume; using speaker default");
        return false;
    }
    if (length != (int)sizeof(record)) {
        MYBOT_LOGW(TAG, "ignored invalid persisted volume length=%d", length);
        return false;
    }

    memcpy(&record, raw, sizeof(record));
    if (record.magic != MYBOT_VOLUME_RECORD_MAGIC ||
        record.version != MYBOT_VOLUME_RECORD_VERSION ||
        record.record_size != sizeof(record) || record.volume < MYBOT_VOLUME_MIN ||
        record.volume > MYBOT_VOLUME_MAX) {
        MYBOT_LOGW(TAG, "ignored invalid persisted volume record");
        return false;
    }

    *stored_volume = record.volume;
    MYBOT_LOGI(TAG, "loaded persisted volume=%d", *stored_volume);
    return true;
}

static int save_volume(int volume) {
    const bk725x_volume_record_t record = {
        .magic = MYBOT_VOLUME_RECORD_MAGIC,
        .version = MYBOT_VOLUME_RECORD_VERSION,
        .record_size = sizeof(record),
        .volume = volume,
    };

    if (bk_set_env_enhance(MYBOT_VOLUME_RECORD_KEY, &record, sizeof(record)) != EF_NO_ERR) {
        MYBOT_LOGE(TAG, "failed to persist volume=%d", volume);
        return -1;
    }
    MYBOT_LOGI(TAG, "persisted volume=%d", volume);
    return 0;
}

/* Adopt a persisted volume as the current known volume.
 * Must be called with s_volume_lock held. */
static void adopt_persisted_volume(int volume)
{
    s_last_volume = volume;
    s_volume_known = true;
    s_persisted_volume = volume;
    s_persisted_volume_known = true;
    s_volume_persist_dirty = false;
}

int mybot_audio_bk725x_volume_init(void **ctx) {
    bk725x_volume_ctx_t *volume = NULL;
    uint8_t gain = 0;
    int stored_volume = 0;
    bool stored_volume_valid;

    if (!ctx) {
        MYBOT_LOGE(TAG, "init failed: invalid output context");
        return -1;
    }
    *ctx = NULL;

    volume = psram_zalloc(sizeof(*volume));
    if (!volume) {
        MYBOT_LOGE(TAG, "init failed: context allocation");
        return -1;
    }

    stored_volume_valid = load_volume(&stored_volume);
    if (ensure_volume_lock() < 0 || rtos_lock_mutex(&s_volume_lock) != BK_OK) {
        MYBOT_LOGE(TAG, "init failed: volume lock");
        goto fail;
    }

    if (!s_volume_known && stored_volume_valid) {
        adopt_persisted_volume(stored_volume);
    }

    if (s_volume_known) {
        gain = volume_to_gain(s_last_volume);
        if (mybot_audio_bk725x_playback_set_digital_gain(gain) < 0) {
            MYBOT_LOGE(TAG, "init failed: restore volume=%d gain=%u", s_last_volume, gain);
            goto unlock_fail;
        }
        volume->volume = s_last_volume;
    } else {
        if (mybot_audio_bk725x_playback_get_digital_gain(&gain) < 0) {
            MYBOT_LOGE(TAG, "init failed: unable to read speaker gain");
            goto unlock_fail;
        }
        volume->volume = gain_to_volume(gain);
        s_last_volume = volume->volume;
        s_volume_known = true;
    }

    volume->magic = MYBOT_VOLUME_CONTEXT_MAGIC;
    *ctx = volume;
    (void)rtos_unlock_mutex(&s_volume_lock);
    MYBOT_LOGI(TAG, "initialized: volume=%d gain=%u", volume->volume, gain);
    return 0;

unlock_fail:
    (void)rtos_unlock_mutex(&s_volume_lock);
fail:
    psram_free(volume);
    return -1;
}

int mybot_audio_bk725x_volume_set(void *ctx, int requested_volume) {
    bk725x_volume_ctx_t *volume = ctx;
    uint8_t gain;

    if (!volume_context_is_valid(volume) || requested_volume < MYBOT_VOLUME_MIN ||
        requested_volume > MYBOT_VOLUME_MAX || !s_volume_lock ||
        rtos_lock_mutex(&s_volume_lock) != BK_OK) {
        MYBOT_LOGE(TAG, "set failed: invalid state or volume=%d", requested_volume);
        return -1;
    }

    gain = volume_to_gain(requested_volume);
    if (mybot_audio_bk725x_playback_set_digital_gain(gain) < 0) {
        MYBOT_LOGE(TAG, "set failed: active speaker unavailable or gain=%u rejected", gain);
        (void)rtos_unlock_mutex(&s_volume_lock);
        return -1;
    }

    volume->volume = requested_volume;
    s_last_volume = requested_volume;
    s_volume_known = true;

    if (s_volume_persist_dirty || !s_persisted_volume_known ||
        s_persisted_volume != requested_volume) {
        if (save_volume(requested_volume) == 0) {
            s_persisted_volume = requested_volume;
            s_persisted_volume_known = true;
            s_volume_persist_dirty = false;
        } else {
            s_volume_persist_dirty = true;
        }
    }

    (void)rtos_unlock_mutex(&s_volume_lock);
    MYBOT_LOGI(TAG, "volume=%d gain=%u", requested_volume, gain);
    return 0;
}

/* Re-apply the currently known volume to whatever playback pipeline is active.
 * A freshly initialized playback pipeline resets its speaker digital gain to the
 * default, so callers that build their own pipeline (e.g. the provisioning PCM
 * prompt player) invoke this right after init to restore the user's volume.
 *
 * The volume may be unknown here: the SDK (and its volume_init) only runs after
 * the WiFi connects, so provisioning reached before any successful connect has
 * never loaded the persisted volume. As a fallback, load it on demand so the
 * prompt still respects the user's last adjustment.
 *
 * Returns 0 when the gain was applied, -1 when no volume is known or persisted
 * and no active playback exists to receive it. */
int mybot_audio_bk725x_volume_apply(void) {
    uint8_t gain;
    MYBOT_LOGI(TAG, "volume apply requested");
    int stored_volume = 0;
    int result = -1;

    if (ensure_volume_lock() < 0 || rtos_lock_mutex(&s_volume_lock) != BK_OK) {
        return -1;
    }
    if (!s_volume_known && load_volume(&stored_volume)) {
        adopt_persisted_volume(stored_volume);
    }
    if (s_volume_known) {
        gain = volume_to_gain(s_last_volume);
        if (mybot_audio_bk725x_playback_set_digital_gain(gain) == 0) {
            result = 0;
        }
    }
    (void)rtos_unlock_mutex(&s_volume_lock);
    MYBOT_LOGI(TAG, "volume apply completed: result=%d known=%d vol=%d", result, s_volume_known, s_last_volume);
    return result;
}

int mybot_audio_bk725x_volume_get(void *ctx, int *current_volume) {
    bk725x_volume_ctx_t *volume = ctx;

    if (!volume_context_is_valid(volume) || !current_volume || !s_volume_lock ||
        rtos_lock_mutex(&s_volume_lock) != BK_OK) {
        return -1;
    }
    if (!mybot_audio_bk725x_playback_is_active()) {
        (void)rtos_unlock_mutex(&s_volume_lock);
        return -1;
    }

    /* Return the exact logical value because adjacent values can map to one gain. */
    *current_volume = volume->volume;
    (void)rtos_unlock_mutex(&s_volume_lock);
    return 0;
}

void mybot_audio_bk725x_volume_destroy(void *ctx) {
    bk725x_volume_ctx_t *volume = ctx;

    if (!volume_context_is_valid(volume)) {
        return;
    }

    /* Playback is destroyed first by the SDK. Retry persistence without touching it. */
    if (s_volume_lock && rtos_lock_mutex(&s_volume_lock) == BK_OK) {
        if (s_volume_known && s_volume_persist_dirty && save_volume(s_last_volume) == 0) {
            s_persisted_volume = s_last_volume;
            s_persisted_volume_known = true;
            s_volume_persist_dirty = false;
        }
        volume->magic = 0;
        (void)rtos_unlock_mutex(&s_volume_lock);
    } else {
        volume->magic = 0;
    }

    psram_free(volume);
    MYBOT_LOGI(TAG, "destroyed");
}
