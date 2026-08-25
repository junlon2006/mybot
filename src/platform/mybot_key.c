/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_key.h>

#include "mybot_key_internal.h"
#include "mybot_platform_registry.h"

#include <stddef.h>

int mybot_key_init(mybot_key_t *key, mybot_key_event_handler_t handler, void *user_data) {
    if (!key || key->active || !mybot_platform_registry_get()->key || !handler) {
        return -1;
    }

    key->ops = mybot_platform_registry_get()->key;
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
