/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_audio_power_bk725x.h"

#include <common/bk_err.h>
#include "mybot_platform_log.h"
#include <modules/wifi.h>
#include <modules/wifi_types.h>
#include <modules/pm.h>

#include <stdbool.h>

#define TAG "mybot_pm"

static bool s_cpu_vote_acquired;
static bool s_audp_vote_acquired;

int mybot_audio_bk725x_power_acquire(void) {
    if (s_cpu_vote_acquired && s_audp_vote_acquired) {
        MYBOT_LOGI(TAG, "power already acquired, no-op");
        return 0;
    }

    if (!s_cpu_vote_acquired) {
        bk_err_t result = bk_pm_module_vote_cpu_freq(PM_DEV_ID_AUDIO, PM_CPU_FRQ_480M);
        if (result != BK_OK) {
            MYBOT_LOGE(TAG, "480 MHz CPU vote failed: %d", result);
            return -1;
        }
        s_cpu_vote_acquired = true;
    }

    if (!s_audp_vote_acquired) {
        bk_err_t result = bk_pm_module_vote_sleep_ctrl(PM_SLEEP_MODULE_NAME_AUDP, 0, 0);
        if (result != BK_OK) {
            MYBOT_LOGE(TAG, "AUDP sleep-disable vote failed: %d", result);
            result = bk_pm_module_vote_cpu_freq(PM_DEV_ID_AUDIO, PM_CPU_FRQ_DEFAULT);
            if (result == BK_OK) {
                s_cpu_vote_acquired = false;
            } else {
                MYBOT_LOGE(TAG, "CPU vote rollback failed: %d", result);
            }
            return -1;
        }
        s_audp_vote_acquired = true;
    }

    bk_err_t result = bk_wifi_set_video_quality(WIFI_VIDEO_QUALITY_FD);
    if (result != BK_OK) {
        MYBOT_LOGW(TAG, "Wi-Fi FD media-quality setting failed: %d", result);
    }

    MYBOT_LOGI(TAG, "active: CPU=480 MHz, AUDP sleep=disabled, Wi-Fi quality=FD");
    return 0;
}

void mybot_audio_bk725x_power_release(void) {
    if (s_cpu_vote_acquired) {
        bk_err_t result = bk_pm_module_vote_cpu_freq(PM_DEV_ID_AUDIO, PM_CPU_FRQ_DEFAULT);
        if (result == BK_OK) {
            s_cpu_vote_acquired = false;
        } else {
            MYBOT_LOGE(TAG, "default CPU vote failed: %d", result);
        }
    }

    if (s_audp_vote_acquired) {
        bk_err_t result = bk_pm_module_vote_sleep_ctrl(PM_SLEEP_MODULE_NAME_AUDP, 1, 0);
        if (result == BK_OK) {
            s_audp_vote_acquired = false;
        } else {
            MYBOT_LOGE(TAG, "AUDP sleep-enable vote failed: %d", result);
        }
    }

    if (!s_cpu_vote_acquired && !s_audp_vote_acquired) {
        MYBOT_LOGI(TAG, "released");
    }
}
