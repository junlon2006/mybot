#include "mybot_wifi_provisioning.h"

#include "api/aosl_atomic.h"
#include "api/aosl_thread.h"

#include <stddef.h>

typedef enum {
    BACKEND_STATE_DISCONNECTED = 0,
    BACKEND_STATE_CONNECTED,
    BACKEND_STATE_FAILED,
} backend_state_t;

static const mybot_wifi_provisioning_ops_t *s_ops;
static void *s_ctx;
static aosl_event_t s_state_changed;
static aosl_atomic_t s_backend_state;
static bool s_active;

static void on_backend_event(mybot_wifi_provisioning_event_t event, void *user_data) {
    (void)user_data;

    switch (event) {
    case MYBOT_WIFI_PROVISIONING_EVENT_STA_CONNECTED:
        aosl_atomic_set(&s_backend_state, BACKEND_STATE_CONNECTED);
        break;
    case MYBOT_WIFI_PROVISIONING_EVENT_STA_DISCONNECTED:
        aosl_atomic_set(&s_backend_state, BACKEND_STATE_DISCONNECTED);
        break;
    case MYBOT_WIFI_PROVISIONING_EVENT_FAILED:
        aosl_atomic_set(&s_backend_state, BACKEND_STATE_FAILED);
        break;
    }
    aosl_event_set(s_state_changed);
}

int mybot_wifi_provisioning_register(const mybot_wifi_provisioning_ops_t *ops) {
    if (!ops || !ops->init || !ops->destroy || s_active) {
        return -1;
    }

    s_ops = ops;
    return 0;
}

int mybot_wifi_provisioning_init(const char *device_id) {
    if (!device_id || !device_id[0] || !s_ops || s_active) {
        return -1;
    }

    s_state_changed = aosl_event_create();
    if (!s_state_changed) {
        return -1;
    }

    aosl_atomic_set(&s_backend_state, BACKEND_STATE_DISCONNECTED);
    if (s_ops->init(&s_ctx, device_id, on_backend_event, NULL) < 0) {
        s_ctx = NULL;
        aosl_event_destroy(s_state_changed);
        s_state_changed = NULL;
        return -1;
    }

    while (aosl_atomic_read(&s_backend_state) == BACKEND_STATE_DISCONNECTED) {
        aosl_event_reset(s_state_changed);
        if (aosl_atomic_read(&s_backend_state) == BACKEND_STATE_DISCONNECTED) {
            aosl_event_wait(s_state_changed);
        }
    }

    if (aosl_atomic_read(&s_backend_state) != BACKEND_STATE_CONNECTED) {
        s_ops->destroy(s_ctx);
        s_ctx = NULL;
        aosl_event_destroy(s_state_changed);
        s_state_changed = NULL;
        return -1;
    }

    s_active = true;
    return 0;
}

bool mybot_wifi_provisioning_is_connected(void) {
    return s_active && aosl_atomic_read(&s_backend_state) == BACKEND_STATE_CONNECTED;
}

void mybot_wifi_provisioning_deinit(void) {
    if (!s_active) {
        return;
    }

    s_ops->destroy(s_ctx);
    s_ctx = NULL;
    aosl_event_destroy(s_state_changed);
    s_state_changed = NULL;
    s_active = false;
    aosl_atomic_set(&s_backend_state, BACKEND_STATE_DISCONNECTED);
}
