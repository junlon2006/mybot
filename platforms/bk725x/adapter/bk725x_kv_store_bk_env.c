/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_kv_store.h>

#include "bk725x_platform_adapters_internal.h"
#include "mybot_kv_bk_env.h"

#include "mybot_platform_log.h"

#include <stdbool.h>
#include <stdint.h>

#define TAG "mybot_kv"

static bool s_kv_store_registered;
static uint8_t s_kv_store_context;

static bool context_is_valid(const void *ctx) {
    return ctx == &s_kv_store_context;
}

static int kv_store_init(void **out_ctx) {
    if (!out_ctx) {
        MYBOT_LOGE(TAG, "init failed: invalid output context");
        return -1;
    }
    *out_ctx = NULL;

    *out_ctx = &s_kv_store_context;
    MYBOT_LOGI(TAG, "initialized");
    return 0;
}

static int kv_store_get(void *opaque, const char *key, void *value, size_t capacity,
                        size_t *out_len) {
    if (!context_is_valid(opaque)) {
        MYBOT_LOGE(TAG, "get failed: invalid request");
        return -1;
    }

    int result = mybot_kv_bk_env_get(key, value, capacity, out_len);
    if (result == MYBOT_KV_BK_ENV_NOT_FOUND) {
        return MYBOT_ERR_NOT_FOUND;
    }
    return result == MYBOT_KV_BK_ENV_OK ? 0 : -1;
}

static int kv_store_set(void *opaque, const char *key, const void *value, size_t len) {
    if (!context_is_valid(opaque)) {
        MYBOT_LOGE(TAG, "set failed: invalid request");
        return -1;
    }
    return mybot_kv_bk_env_set(key, value, len) == MYBOT_KV_BK_ENV_OK ? 0 : -1;
}

static int kv_store_erase(void *opaque, const char *key) {
    if (!context_is_valid(opaque)) {
        MYBOT_LOGE(TAG, "erase failed: invalid request");
        return -1;
    }
    return mybot_kv_bk_env_erase(key) == MYBOT_KV_BK_ENV_OK ? 0 : -1;
}

static void kv_store_destroy(void *opaque) {
    if (!context_is_valid(opaque)) {
        return;
    }
    MYBOT_LOGI(TAG, "destroyed");
}

static const mybot_kv_store_ops_t s_kv_store_ops = {
    .name = "bk725x-env",
    .init = kv_store_init,
    .get = kv_store_get,
    .set = kv_store_set,
    .erase = kv_store_erase,
    .destroy = kv_store_destroy,
};

int bk725x_kv_store_platform_register_bk_env(void) {
    if (s_kv_store_registered) {
        return 0;
    }
    if (mybot_kv_store_register(&s_kv_store_ops) < 0) {
        MYBOT_LOGE(TAG, "registration failed");
        return -1;
    }

    s_kv_store_registered = true;
    MYBOT_LOGI(TAG, "backend ready: %s", s_kv_store_ops.name);
    return 0;
}
