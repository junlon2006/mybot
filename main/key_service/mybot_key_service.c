#include "mybot_key_service.h"

#include <stddef.h>

static const mybot_key_service_ops_t *s_ops;
static void *s_ctx;
static mybot_key_event_handler_t s_handler;
static void *s_user_data;
static int s_active;

static void emit_event(mybot_key_event_t event, void *user_data) {
    (void)user_data;
    if (s_active && s_handler) {
        s_handler(event, s_user_data);
    }
}

int mybot_key_service_register(const mybot_key_service_ops_t *ops) {
    if (!ops || !ops->init || !ops->destroy || s_active) {
        return -1;
    }
    s_ops = ops;
    return 0;
}

int mybot_key_service_init(mybot_key_event_handler_t handler, void *user_data) {
    if (s_active || !s_ops || !handler) {
        return -1;
    }

    s_handler = handler;
    s_user_data = user_data;
    if (s_ops->init(&s_ctx, emit_event, NULL) < 0) {
        s_ctx = NULL;
        s_handler = NULL;
        s_user_data = NULL;
        return -1;
    }
    s_active = 1;
    return 0;
}

int mybot_key_service_poll(void) {
    if (!s_active) {
        return -1;
    }
    return s_ops->poll ? s_ops->poll(s_ctx) : 0;
}

void mybot_key_service_deinit(void) {
    if (!s_active) {
        return;
    }

    s_active = 0;
    s_ops->destroy(s_ctx);
    s_ctx = NULL;
    s_handler = NULL;
    s_user_data = NULL;
}
