/* SPDX-License-Identifier: Apache-2.0 */
#include "bk725x_platform_adapters_internal.h"
#include "mybot_announce_pcm_bk725x.h"

#include <mybot/platform/mybot_announce.h>

#include "mybot_platform_log.h"

#include <stdbool.h>

#define TAG "mybot_announce"

static bool s_announce_registered;

static const mybot_announce_ops_t s_bk725x_announce_ops = {
    .name = "bk725x-sdcard-pcm",
    .init = mybot_announce_pcm_bk725x_init,
    .open = mybot_announce_pcm_bk725x_open,
    .read = mybot_announce_pcm_bk725x_read,
    .close = mybot_announce_pcm_bk725x_close,
    .destroy = mybot_announce_pcm_bk725x_destroy,
};

int bk725x_announce_platform_register_pcm(void) {
    if (s_announce_registered) {
        return 0;
    }

    int result = mybot_announce_register(&s_bk725x_announce_ops);
    if (result < 0) {
        MYBOT_LOGE(TAG, "registration failed");
        return result;
    }

    s_announce_registered = true;
    MYBOT_LOGI(TAG, "registered backend=%s", s_bk725x_announce_ops.name);
    return 0;
}
