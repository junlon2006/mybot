/* SPDX-License-Identifier: Apache-2.0 */
#include "bk725x_platform_adapters_internal.h"

#include "mybot_https_bk725x.h"

#include "mybot_platform_log.h"
#include <mybot/platform/mybot_https.h>

#define TAG "mybot_https"

static const mybot_https_ops_t s_https_ops = {
    .connect = mybot_https_bk725x_connect,
    .send = mybot_https_bk725x_send,
    .recv = mybot_https_bk725x_recv,
    .close = mybot_https_bk725x_close,
};

const mybot_https_ops_t *bk725x_https_platform_ops_mbedtls(void) {
    return &s_https_ops;
}
