#include "wifi/mybot_wifi_provisioning.h"

#include "api/aosl_log.h"

#include <stdlib.h>

typedef struct {
    mybot_wifi_provisioning_event_handler_t emit;
    void *user_data;
} linux_wifi_host_network_ctx_t;

static int wifi_host_network_init(void **out_ctx, const char *device_id,
                                  mybot_wifi_provisioning_event_handler_t emit, void *user_data) {
    if (!out_ctx || !device_id || !device_id[0] || !emit) {
        return -1;
    }

    linux_wifi_host_network_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return -1;
    }

    ctx->emit = emit;
    ctx->user_data = user_data;
    *out_ctx = ctx;

    AOSL_LOG_INF("wifi provisioning: using Linux host network for device %s", device_id);
    ctx->emit(MYBOT_WIFI_PROVISIONING_EVENT_STA_CONNECTED, ctx->user_data);
    return 0;
}

static void wifi_host_network_destroy(void *opaque) {
    free(opaque);
}

static const mybot_wifi_provisioning_ops_t s_wifi_host_network_ops = {
    .name = "linux-host-network",
    .init = wifi_host_network_init,
    .destroy = wifi_host_network_destroy,
};

void linux_wifi_platform_register_host_network(void) {
    if (mybot_wifi_provisioning_register(&s_wifi_host_network_ops) < 0) {
        AOSL_LOG_ERR("wifi provisioning platform registration failed");
    }
}
