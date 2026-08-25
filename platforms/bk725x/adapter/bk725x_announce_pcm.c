/* SPDX-License-Identifier: Apache-2.0 */
#include "bk725x_platform_adapters_internal.h"
#include "mybot_announce_pcm_bk725x.h"

#include <mybot/platform/mybot_announce.h>

#include "mybot_platform_log.h"

#define TAG "mybot_announce"

static const mybot_announce_ops_t s_bk725x_announce_ops = {
    .init = mybot_announce_pcm_bk725x_init,
    .open = mybot_announce_pcm_bk725x_open,
    .read = mybot_announce_pcm_bk725x_read,
    .close = mybot_announce_pcm_bk725x_close,
    .destroy = mybot_announce_pcm_bk725x_destroy,
};

const mybot_announce_ops_t *bk725x_announce_platform_ops_pcm(void) {
    return &s_bk725x_announce_ops;
}
