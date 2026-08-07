/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_wifi.h>

#include "api/aosl_atomic.h"

#include <stddef.h>

static const mybot_wifi_provisioning_ops_t *s_ops;
static void *s_ctx;
static mybot_wifi_provisioning_state_handler_t s_handler;
static void *s_user_data;
static aosl_atomic_t s_state;
static bool s_active;

static void set_state(mybot_wifi_provisioning_state_t state) {
    mybot_wifi_provisioning_state_t old =
        (mybot_wifi_provisioning_state_t)aosl_atomic_xchg(&s_state, state);
    if (state != old && s_handler) {
        s_handler(state, s_user_data);
    }
}

static void on_backend_event(mybot_wifi_provisioning_event_t event, void *user_data) {
    (void)user_data;

    switch (event) {
    case MYBOT_WIFI_PROVISIONING_EVENT_STA_CONNECTED:
        set_state(MYBOT_WIFI_PROVISIONING_STATE_CONNECTED);
        break;
    case MYBOT_WIFI_PROVISIONING_EVENT_STA_DISCONNECTED:
        set_state(MYBOT_WIFI_PROVISIONING_STATE_DISCONNECTED);
        break;
    case MYBOT_WIFI_PROVISIONING_EVENT_FAILED:
        set_state(MYBOT_WIFI_PROVISIONING_STATE_FAILED);
        break;
    }
}

int mybot_wifi_provisioning_register(const mybot_wifi_provisioning_ops_t *ops) {
    if (!ops || !ops->init || !ops->destroy || s_active) {
        return -1;
    }

    s_ops = ops;
    return 0;
}

int mybot_wifi_provisioning_init(const char *device_id,
                                 mybot_wifi_provisioning_state_handler_t handler, void *user_data) {
    if (!device_id || !device_id[0] || !s_ops || s_active) {
        return -1;
    }

    s_handler = handler;
    s_user_data = user_data;
    aosl_atomic_set(&s_state, MYBOT_WIFI_PROVISIONING_STATE_PROVISIONING);
    if (s_ops->init(&s_ctx, device_id, on_backend_event, NULL) < 0) {
        s_ctx = NULL;
        s_handler = NULL;
        s_user_data = NULL;
        aosl_atomic_set(&s_state, MYBOT_WIFI_PROVISIONING_STATE_FAILED);
        return -1;
    }

    s_active = true;
    return 0;
}

mybot_wifi_provisioning_state_t mybot_wifi_provisioning_get_state(void) {
    return (mybot_wifi_provisioning_state_t)aosl_atomic_read(&s_state);
}

void mybot_wifi_provisioning_deinit(void) {
    if (!s_active) {
        return;
    }

    s_ops->destroy(s_ctx);
    s_ctx = NULL;
    s_active = false;
    s_handler = NULL;
    s_user_data = NULL;
    aosl_atomic_set(&s_state, MYBOT_WIFI_PROVISIONING_STATE_IDLE);
}
