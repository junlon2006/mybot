/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_key.h>

#include "mybot_key_internal.h"

#include <stddef.h>

static const mybot_key_ops_t *s_ops;
static void *s_ctx;
static int s_active;

int mybot_key_register(const mybot_key_ops_t *ops) {
    if (!ops || !ops->init || !ops->destroy || s_active) {
        return -1;
    }
    s_ops = ops;
    return 0;
}

int mybot_key_init(mybot_key_event_handler_t handler, void *user_data) {
    if (s_active || !s_ops || !handler) {
        return -1;
    }

    if (s_ops->init(&s_ctx, handler, user_data) < 0) {
        s_ctx = NULL;
        return -1;
    }
    s_active = 1;
    return 0;
}

void mybot_key_deinit(void) {
    if (!s_active) {
        return;
    }

    s_ops->destroy(s_ctx);
    s_ctx = NULL;
    s_active = 0;
}
