/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_platform_registry.h"

#include <assert.h>

static int wifi_init(void **ctx, const char *device_id, mybot_wifi_event_handler_t emit,
                     void *user_data) {
    (void)device_id;
    (void)emit;
    (void)user_data;
    *ctx = ctx;
    return 0;
}

static void wifi_destroy(void *ctx) {
    (void)ctx;
}

int main(void) {
    const mybot_wifi_ops_t unnamed_wifi = {
        .init = wifi_init,
        .destroy = wifi_destroy,
    };

    assert(mybot_platform_registry_register_wifi(&unnamed_wifi) == 0);
    assert(mybot_platform_get_capabilities() == MYBOT_PLATFORM_CAP_WIFI);
    assert(mybot_platform_registry_wifi() == &unnamed_wifi);
    assert(mybot_platform_registry_register_wifi(&unnamed_wifi) < 0);
    return 0;
}
