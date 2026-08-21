/* SPDX-License-Identifier: Apache-2.0 */
#include "bk725x_platform_adapters_internal.h"

#include "mybot_https_bk725x.h"

#include "mybot_platform_log.h"
#include <mybot/platform/mybot_https.h>

#include <stdbool.h>

#define TAG "mybot_https"

static bool s_https_registered;

static const mybot_https_ops_t s_https_ops = {
    .name = "bk725x-mbedtls",
    .connect = mybot_https_bk725x_connect,
    .send = mybot_https_bk725x_send,
    .recv = mybot_https_bk725x_recv,
    .close = mybot_https_bk725x_close,
};

int bk725x_https_platform_register_mbedtls(void) {
    if (s_https_registered) {
        return 0;
    }
    if (mybot_https_register(&s_https_ops) < 0) {
        MYBOT_LOGE(TAG, "registration failed");
        return -1;
    }

    s_https_registered = true;
    MYBOT_LOGW(TAG, "backend ready without CA verification: %s", s_https_ops.name);
    return 0;
}
