/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_sdcard_msc_bk725x.h"
#include "mybot_sdcard_bk725x.h"

#include "sdkconfig.h"

#include <common/bk_err.h>
#include "mybot_platform_log.h"

#include <stdbool.h>

#define TAG "mybot_msc"

#if CONFIG_USBD_MSC
static bool s_initialized;
#endif

int mybot_sdcard_msc_bk725x_init(void) {
#if CONFIG_USBD_MSC
    extern int msc_storage_init(void);

    if (s_initialized) {
        return 0;
    }

    /* Hold an SD card reference while USB mass storage is active so the
     * FATFS mount is never released from under the host. */
    if (mybot_sdcard_bk725x_acquire() < 0) {
        MYBOT_LOGE(TAG, "SD card access unavailable, cannot start USB MSC");
        return -1;
    }

    MYBOT_LOGI(TAG, "initializing SD card USB mass storage");
    int result = msc_storage_init();
    if (result != BK_OK) {
        MYBOT_LOGE(TAG, "SD card USB mass storage initialization failed: %d", result);
        mybot_sdcard_bk725x_release();
        return -1;
    }

    s_initialized = true;
    MYBOT_LOGI(TAG, "SD card USB mass storage ready");
    return 0;
#else
    MYBOT_LOGW(TAG, "USB device MSC is disabled");
    return -1;
#endif
}
