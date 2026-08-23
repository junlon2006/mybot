/* SPDX-License-Identifier: Apache-2.0 */
#include "linux_platform.h"

#include <mybot/mybot_build_config.h>
#include <mybot/platform/mybot_platform.h>

#include <assert.h>

int main(void) {
    uint64_t expected = MYBOT_PLATFORM_CAP_REQUIRED | MYBOT_PLATFORM_CAP_AUDIO_VOLUME |
                        MYBOT_PLATFORM_CAP_LCD | MYBOT_PLATFORM_CAP_ANNOUNCE;
#if MYBOT_LINUX_HTTPS_OPENSSL
    expected |= MYBOT_PLATFORM_CAP_HTTPS;
#endif

    assert(linux_platform_register() == 0);
    assert(mybot_platform_get_capabilities() == expected);

    uint64_t missing = UINT64_MAX;
    assert(mybot_platform_validate(expected, &missing) == 0);
    assert(missing == 0);
    assert(linux_platform_register() < 0);
    return 0;
}
