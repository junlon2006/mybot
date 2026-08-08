/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_KV_STORE_H_
#define MYBOT_KV_STORE_H_

#include <stddef.h>
#include <mybot/mybot_errors.h>
#include <mybot/mybot_export.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    int (*init)(void **ctx);
    int (*get)(void *ctx, const char *key, void *value, size_t capacity, size_t *out_len);
    int (*set)(void *ctx, const char *key, const void *value, size_t len);
    int (*erase)(void *ctx, const char *key);
    void (*destroy)(void *ctx);
} mybot_kv_store_ops_t;

MYBOT_API int mybot_kv_store_register(const mybot_kv_store_ops_t *ops);
MYBOT_API int mybot_kv_store_init(void);
MYBOT_API void mybot_kv_store_deinit(void);
MYBOT_API int mybot_kv_store_get(const char *key, void *value, size_t capacity, size_t *out_len);
MYBOT_API int mybot_kv_store_set(const char *key, const void *value, size_t len);
MYBOT_API int mybot_kv_store_erase(const char *key);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_KV_STORE_H_ */
