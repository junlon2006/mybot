/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_wifi.h>

#include "bk725x_platform_adapters_internal.h"
#include "mybot_connectivity.h"

#include "mybot_platform_log.h"

#define TAG "mybot_wifi"

typedef struct {
    mybot_wifi_event_handler_t emit;
    void *user_data;
} wifi_adapter_binding_t;

static wifi_adapter_binding_t s_binding;

static void connectivity_event_bridge(mybot_connectivity_event_t event, void *user_data) {
    wifi_adapter_binding_t *binding = user_data;
    mybot_wifi_event_t sdk_event;

    if (!binding || !binding->emit) {
        return;
    }

    switch (event) {
    case MYBOT_CONNECTIVITY_CONNECTED:
        sdk_event = MYBOT_WIFI_EVENT_STA_CONNECTED;
        break;
    case MYBOT_CONNECTIVITY_DISCONNECTED:
        sdk_event = MYBOT_WIFI_EVENT_STA_DISCONNECTED;
        break;
    case MYBOT_CONNECTIVITY_FAILED:
        sdk_event = MYBOT_WIFI_EVENT_FAILED;
        break;
    default:
        return;
    }
    binding->emit(sdk_event, binding->user_data);
}

static int wifi_adapter_init(void **ctx, const char *device_id,
                             mybot_wifi_event_handler_t emit, void *user_data) {
    if (!ctx || !device_id || !device_id[0] || !emit || s_binding.emit) {
        return -1;
    }

    s_binding.emit = emit;
    s_binding.user_data = user_data;
    if (mybot_connectivity_subscribe(ctx, connectivity_event_bridge, &s_binding) < 0) {
        s_binding = (wifi_adapter_binding_t){0};
        return -1;
    }
    return 0;
}

static void wifi_adapter_destroy(void *ctx) {
    if (mybot_connectivity_unsubscribe(ctx) < 0) {
        MYBOT_LOGE(TAG, "failed to remove connectivity subscription");
        return;
    }
    s_binding = (wifi_adapter_binding_t){0};
}

static const mybot_wifi_ops_t s_ops = {
    .init = wifi_adapter_init,
    .destroy = wifi_adapter_destroy,
};

const mybot_wifi_ops_t *bk725x_wifi_platform_ops_connectivity(void) {
    return &s_ops;
}
