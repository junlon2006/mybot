/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_sdcard_bk725x.h"

#include "bk_partition.h"
#include "bk_posix.h"
#include "sdkconfig.h"

#include <common/bk_err.h>
#include "mybot_platform_log.h"
#include <os/os.h>

#include <stdbool.h>

#define TAG "mybot_sd"

static beken_mutex_t s_sdcard_lock;
static int s_refcount;

static int ensure_lock(void) {
    if (s_sdcard_lock) {
        return 0;
    }
    if (rtos_init_mutex(&s_sdcard_lock) != BK_OK) {
        MYBOT_LOGE(TAG, "failed to initialize SD card lock");
        return -1;
    }
    return 0;
}

static int do_mount(void) {
    struct bk_fatfs_partition partition = {
        .part_type = FATFS_DEVICE,
        .mount_path = VFS_SD_0_PATITION_0,
        .part_dev.device_name = FATFS_DEV_SDCARD,
    };

    if (mount("SOURCE_NONE", partition.mount_path, "fatfs", 0, &partition) != BK_OK) {
        MYBOT_LOGW(TAG, "failed to mount SD card at %s", partition.mount_path);
        return -1;
    }
    MYBOT_LOGI(TAG, "SD card mounted at %s", partition.mount_path);
    return 0;
}

static void do_unmount(void) {
    if (umount(VFS_SD_0_PATITION_0) != BK_OK) {
        MYBOT_LOGW(TAG, "failed to unmount SD card from %s", VFS_SD_0_PATITION_0);
    } else {
        MYBOT_LOGI(TAG, "SD card unmounted from %s", VFS_SD_0_PATITION_0);
    }
}

int mybot_sdcard_bk725x_acquire(void) {
    if (ensure_lock() < 0 || rtos_lock_mutex(&s_sdcard_lock) != BK_OK) {
        return -1;
    }

    if (s_refcount == 0) {
        if (do_mount() < 0) {
            (void)rtos_unlock_mutex(&s_sdcard_lock);
            return -1;
        }
    }

    ++s_refcount;
    MYBOT_LOGI(TAG, "acquired (refcount=%d)", s_refcount);
    (void)rtos_unlock_mutex(&s_sdcard_lock);
    return 0;
}

void mybot_sdcard_bk725x_release(void) {
    if (ensure_lock() < 0 || rtos_lock_mutex(&s_sdcard_lock) != BK_OK) {
        return;
    }

    if (s_refcount == 0) {
        (void)rtos_unlock_mutex(&s_sdcard_lock);
        return;
    }

    --s_refcount;
    MYBOT_LOGI(TAG, "released (refcount=%d)", s_refcount);

    if (s_refcount == 0) {
        do_unmount();
    }

    (void)rtos_unlock_mutex(&s_sdcard_lock);
}

bool mybot_sdcard_bk725x_is_mounted(void) {
    bool mounted = false;

    if (ensure_lock() < 0 || rtos_lock_mutex(&s_sdcard_lock) != BK_OK) {
        return false;
    }
    mounted = s_refcount > 0;
    (void)rtos_unlock_mutex(&s_sdcard_lock);
    return mounted;
}
