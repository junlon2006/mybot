/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_wifi.h>

#include "mybot_wifi_internal.h"

#include <stdbool.h>
#include <stddef.h>

static const mybot_wifi_ops_t *s_ops;
static void *s_ctx;
static bool s_active;

int mybot_wifi_register(const mybot_wifi_ops_t *ops) {
    if (!ops || !ops->init || !ops->destroy || s_active) {
        return -1;
    }

    s_ops = ops;
    return 0;
}

int mybot_wifi_init(const char *device_id, mybot_wifi_event_handler_t handler, void *user_data) {
    if (!device_id || !device_id[0] || !handler || !s_ops || s_active) {
        return -1;
    }

    if (s_ops->init(&s_ctx, device_id, handler, user_data) < 0) {
        s_ctx = NULL;
        return -1;
    }

    s_active = true;
    return 0;
}

void mybot_wifi_deinit(void) {
    if (!s_active) {
        return;
    }

    s_ops->destroy(s_ctx);
    s_ctx = NULL;
    s_active = false;
}
