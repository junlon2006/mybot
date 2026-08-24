/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_KV_STORE_INTERNAL_H_
#define MYBOT_KV_STORE_INTERNAL_H_

#include <mybot/platform/mybot_kv_store.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const mybot_kv_store_ops_t *ops;
    void *ctx;
} mybot_kv_store_t;

/**
 * SDK-internal KV-store facade. The public mybot/platform/mybot_kv_store.h
 * only exposes the platform contract (ops table + mybot_platform_register());
 * the SDK core uses the storage operations below, e.g. to persist device
 * credentials. Application code has no documented access to this store.
 */

int mybot_kv_store_init(mybot_kv_store_t *store);
void mybot_kv_store_deinit(mybot_kv_store_t *store);
int mybot_kv_store_get(mybot_kv_store_t *store, const char *key, void *value, size_t capacity,
                       size_t *out_len);
int mybot_kv_store_set(mybot_kv_store_t *store, const char *key, const void *value, size_t len);
int mybot_kv_store_erase(mybot_kv_store_t *store, const char *key);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_KV_STORE_INTERNAL_H_ */
