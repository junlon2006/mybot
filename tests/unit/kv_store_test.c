/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_kv_store_internal.h"
#include "platform_test.h"

#include <assert.h>
#include <string.h>

static int s_context;
static int s_init_calls;
static int s_destroy_calls;
static int s_operation_calls;
static int s_init_result;
static int s_operation_result;
static const char s_value[] = "credential";

static int store_init(void **ctx) {
    s_init_calls++;
    if (s_init_result == 0) {
        *ctx = &s_context;
    }
    return s_init_result;
}

static int store_get(void *ctx, const char *key, void *value, size_t capacity, size_t *out_len) {
    assert(ctx == &s_context && strcmp(key, "token") == 0);
    s_operation_calls++;
    if (s_operation_result != 0) {
        return s_operation_result;
    }
    if (capacity < sizeof(s_value)) {
        return -1;
    }
    memcpy(value, s_value, sizeof(s_value));
    *out_len = sizeof(s_value);
    return 0;
}

static int store_set(void *ctx, const char *key, const void *value, size_t len) {
    assert(ctx == &s_context && strcmp(key, "token") == 0);
    assert(len == 0 || (len == sizeof(s_value) && memcmp(value, s_value, len) == 0));
    s_operation_calls++;
    return s_operation_result;
}

static int store_erase(void *ctx, const char *key) {
    assert(ctx == &s_context && strcmp(key, "token") == 0);
    s_operation_calls++;
    return s_operation_result;
}

static void store_destroy(void *ctx) {
    assert(ctx == &s_context);
    s_destroy_calls++;
}

int main(void) {
    static const mybot_kv_store_ops_t ops = {
        .init = store_init,
        .get = store_get,
        .set = store_set,
        .erase = store_erase,
        .destroy = store_destroy,
    };
    mybot_kv_store_t store = {0};
    char value[sizeof(s_value)] = {0};
    size_t out_len = 99;
    assert(mybot_kv_store_init(NULL) < 0);
    assert(mybot_kv_store_init(&store) < 0);
    assert(mybot_kv_store_get(&store, "token", value, sizeof(value), &out_len) < 0);
    assert(mybot_kv_store_set(&store, "token", s_value, sizeof(s_value)) < 0);
    assert(mybot_kv_store_erase(&store, "token") < 0);
    assert(out_len == 99 && s_operation_calls == 0);
    mybot_kv_store_deinit(NULL);
    mybot_kv_store_deinit(&store);

    mybot_platform_descriptor_t descriptor = mybot_test_platform_descriptor();
    descriptor.kv_store = &ops;
    assert(mybot_platform_register(&descriptor) == 0);
    s_init_result = -1;
    assert(mybot_kv_store_init(&store) < 0 && store.ctx == NULL);
    mybot_kv_store_deinit(&store);
    assert(s_init_calls == 1 && s_destroy_calls == 0);
    s_init_result = 0;
    assert(mybot_kv_store_init(&store) == 0 && store.ctx == &s_context);
    assert(mybot_kv_store_init(&store) == 0 && s_init_calls == 2);

    assert(mybot_kv_store_get(NULL, "token", value, sizeof(value), &out_len) < 0);
    assert(mybot_kv_store_get(&store, NULL, value, sizeof(value), &out_len) < 0);
    assert(mybot_kv_store_get(&store, "token", NULL, sizeof(value), &out_len) < 0);
    assert(mybot_kv_store_get(&store, "token", value, sizeof(value), NULL) < 0);
    assert(mybot_kv_store_set(NULL, "token", s_value, sizeof(s_value)) < 0);
    assert(mybot_kv_store_set(&store, NULL, s_value, sizeof(s_value)) < 0);
    assert(mybot_kv_store_set(&store, "token", NULL, 1) < 0);
    assert(mybot_kv_store_erase(NULL, "token") < 0);
    assert(mybot_kv_store_erase(&store, NULL) < 0);
    assert(s_operation_calls == 0 && out_len == 99);

    const int errors[] = {MYBOT_ERR_NOT_FOUND, -1};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        s_operation_result = errors[i];
        assert(mybot_kv_store_get(&store, "token", value, sizeof(value), &out_len) == errors[i]);
        assert(out_len == 99 && value[0] == '\0');
    }
    s_operation_result = -1;
    assert(mybot_kv_store_set(&store, "token", s_value, sizeof(s_value)) == -1);
    assert(mybot_kv_store_erase(&store, "token") == -1);
    s_operation_result = 0;
    assert(mybot_kv_store_get(&store, "token", value, sizeof(value) - 1, &out_len) < 0);
    assert(out_len == 99 && value[0] == '\0');
    assert(mybot_kv_store_get(&store, "token", value, sizeof(value), &out_len) == 0);
    assert(out_len == sizeof(s_value) && memcmp(value, s_value, sizeof(value)) == 0);
    assert(mybot_kv_store_set(&store, "token", s_value, sizeof(s_value)) == 0);
    assert(mybot_kv_store_set(&store, "token", NULL, 0) == 0);
    assert(mybot_kv_store_erase(&store, "token") == 0);
    assert(mybot_kv_store_erase(&store, "token") == 0);

    mybot_kv_store_deinit(&store);
    mybot_kv_store_deinit(&store);
    assert(store.ctx == NULL && s_destroy_calls == 1);
    int calls_before = s_operation_calls;
    assert(mybot_kv_store_get(&store, "token", value, sizeof(value), &out_len) < 0);
    assert(mybot_kv_store_set(&store, "token", s_value, sizeof(s_value)) < 0);
    assert(mybot_kv_store_erase(&store, "token") < 0);
    assert(s_operation_calls == calls_before);
    assert(mybot_kv_store_init(&store) == 0 && s_init_calls == 3);
    mybot_kv_store_deinit(&store);
    assert(s_destroy_calls == 2);
    return 0;
}
