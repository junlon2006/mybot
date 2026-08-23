/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_key.h>

#include "mybot_key_internal.h"

#include <stddef.h>

static const mybot_key_ops_t *s_registered_ops;

int mybot_key_register(const mybot_key_ops_t *ops) {
    if (!ops || !ops->init || !ops->destroy || s_registered_ops) {
        return -1;
    }
    s_registered_ops = ops;
    return 0;
}

int mybot_key_init(mybot_key_t *key, mybot_key_event_handler_t handler, void *user_data) {
    if (!key || key->active || !s_registered_ops || !handler) {
        return -1;
    }

    key->ops = s_registered_ops;
    if (key->ops->init(&key->ctx, handler, user_data) < 0) {
        key->ctx = NULL;
        return -1;
    }
    key->active = true;
    return 0;
}

void mybot_key_deinit(mybot_key_t *key) {
    if (!key || !key->active) {
        return;
    }

    key->ops->destroy(key->ctx);
    key->ctx = NULL;
    key->active = false;
}
