/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_KV_BK_ENV_H_
#define MYBOT_KV_BK_ENV_H_

#include <stddef.h>

typedef enum {
    MYBOT_KV_BK_ENV_ERROR = -1,
    MYBOT_KV_BK_ENV_OK = 0,
    MYBOT_KV_BK_ENV_NOT_FOUND = 1,
} mybot_kv_bk_env_result_t;

int mybot_kv_bk_env_get(const char *key, void *value, size_t capacity,
                        size_t *out_len);
int mybot_kv_bk_env_set(const char *key, const void *value, size_t len);
int mybot_kv_bk_env_erase(const char *key);

#endif /* MYBOT_KV_BK_ENV_H_ */
