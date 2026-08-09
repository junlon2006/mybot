/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_kv_store.h>

#include "mybot_kv_store_internal.h"

static const mybot_kv_store_ops_t *s_ops;
static void *s_ctx;

int mybot_kv_store_register(const mybot_kv_store_ops_t *ops) {
    if (!ops || !ops->init || !ops->get || !ops->set || !ops->erase || !ops->destroy) {
        return -1;
    }
    if (s_ctx) {
        return -1;
    }
    s_ops = ops;
    return 0;
}

int mybot_kv_store_init(void) {
    if (s_ctx) {
        return 0;
    }
    if (!s_ops) {
        return -1;
    }
    return s_ops->init(&s_ctx);
}

void mybot_kv_store_deinit(void) {
    if (s_ctx) {
        s_ops->destroy(s_ctx);
        s_ctx = NULL;
    }
}

int mybot_kv_store_get(const char *key, void *value, size_t capacity, size_t *out_len) {
    if (!s_ctx || !key || !value || !out_len) {
        return -1;
    }
    return s_ops->get(s_ctx, key, value, capacity, out_len);
}

int mybot_kv_store_set(const char *key, const void *value, size_t len) {
    if (!s_ctx || !key || (!value && len != 0)) {
        return -1;
    }
    return s_ops->set(s_ctx, key, value, len);
}

int mybot_kv_store_erase(const char *key) {
    if (!s_ctx || !key) {
        return -1;
    }
    return s_ops->erase(s_ctx, key);
}
