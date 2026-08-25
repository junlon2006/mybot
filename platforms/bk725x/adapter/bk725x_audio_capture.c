/* SPDX-License-Identifier: Apache-2.0 */
#include "bk725x_platform_adapters_internal.h"
#include "mybot_audio_capture_bk725x.h"

#include <mybot/platform/mybot_audio.h>

#include "mybot_platform_log.h"

#define TAG "mybot_cap"

static const mybot_audio_capture_ops_t s_bk725x_capture_ops = {
    .init = mybot_audio_bk725x_capture_init,
    .start = mybot_audio_bk725x_capture_start,
    .read = mybot_audio_bk725x_capture_read,
    .stop = mybot_audio_bk725x_capture_stop,
    .destroy = mybot_audio_bk725x_capture_destroy,
};

const mybot_audio_capture_ops_t *bk725x_audio_platform_ops_capture(void) {
    return &s_bk725x_capture_ops;
}
