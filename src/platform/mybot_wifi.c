/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_wifi.h>

#include "mybot_wifi_internal.h"
#include "mybot_platform_registry.h"

#include <stdbool.h>
#include <stddef.h>

int mybot_wifi_init(mybot_wifi_t *wifi, const char *device_id, mybot_wifi_event_handler_t handler,
                    void *user_data) {
    if (!wifi || !device_id || !device_id[0] || !handler || !mybot_platform_registry_get()->wifi ||
        wifi->active) {
        return -1;
    }

    wifi->ops = mybot_platform_registry_get()->wifi;
    if (wifi->ops->init(&wifi->ctx, device_id, handler, user_data) < 0) {
        wifi->ctx = NULL;
        return -1;
    }

    wifi->active = true;
    return 0;
}

void mybot_wifi_deinit(mybot_wifi_t *wifi) {
    if (!wifi || !wifi->active) {
        return;
    }

    wifi->ops->destroy(wifi->ctx);
    wifi->ctx = NULL;
    wifi->active = false;
}
