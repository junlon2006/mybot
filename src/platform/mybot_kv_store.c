/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_kv_store.h>

#include "mybot_kv_store_internal.h"
#include "mybot_platform_registry.h"

int mybot_kv_store_init(mybot_kv_store_t *store) {
    if (!store) {
        return -1;
    }
    if (store->ctx) {
        return 0;
    }
    store->ops = mybot_platform_registry_get()->kv_store;
    if (!store->ops) {
        return -1;
    }
    return store->ops->init(&store->ctx);
}

void mybot_kv_store_deinit(mybot_kv_store_t *store) {
    if (store && store->ctx) {
        store->ops->destroy(store->ctx);
        store->ctx = NULL;
    }
}

int mybot_kv_store_get(mybot_kv_store_t *store, const char *key, void *value, size_t capacity,
                       size_t *out_len) {
    if (!store || !store->ctx || !key || !value || !out_len) {
        return -1;
    }
    return store->ops->get(store->ctx, key, value, capacity, out_len);
}

int mybot_kv_store_set(mybot_kv_store_t *store, const char *key, const void *value, size_t len) {
    if (!store || !store->ctx || !key || (!value && len != 0)) {
        return -1;
    }
    return store->ops->set(store->ctx, key, value, len);
}

int mybot_kv_store_erase(mybot_kv_store_t *store, const char *key) {
    if (!store || !store->ctx || !key) {
        return -1;
    }
    return store->ops->erase(store->ctx, key);
}
