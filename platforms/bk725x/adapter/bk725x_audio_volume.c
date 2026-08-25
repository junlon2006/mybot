/* SPDX-License-Identifier: Apache-2.0 */
#include "bk725x_platform_adapters_internal.h"
#include "mybot_audio_volume_bk725x.h"

#include <mybot/platform/mybot_audio.h>

#include "mybot_platform_log.h"

#define TAG "mybot_vol"

static const mybot_audio_volume_ops_t s_bk725x_volume_ops = {
    .init = mybot_audio_bk725x_volume_init,
    .set_volume = mybot_audio_bk725x_volume_set,
    .get_volume = mybot_audio_bk725x_volume_get,
    .destroy = mybot_audio_bk725x_volume_destroy,
};

const mybot_audio_volume_ops_t *bk725x_audio_platform_ops_volume(void) {
    return &s_bk725x_volume_ops;
}
