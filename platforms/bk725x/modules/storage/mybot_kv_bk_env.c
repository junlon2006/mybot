/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_kv_bk_env.h"

#include <bk_ef.h>
#include "mybot_platform_log.h"
#include <os/mem.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TAG "mybot_kv"

#define MYBOT_KV_KEY_PREFIX "mybot_"
#define MYBOT_KV_RECORD_MAGIC 0x4d424b56u
#define MYBOT_KV_RECORD_VERSION 1u
#define MYBOT_KV_VALUE_MAX 2048u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t value_size;
    uint8_t value[];
} mybot_kv_record_t;

_Static_assert(sizeof(mybot_kv_record_t) == 12, "kv record header size");

static bool build_storage_key(const char *key, char storage_key[EF_ENV_NAME_MAX + 1]) {
    static const char prefix[] = MYBOT_KV_KEY_PREFIX;
    const size_t prefix_len = sizeof(prefix) - 1;

    if (!key || !key[0]) {
        return false;
    }

    size_t key_len = 0;
    for (const unsigned char *p = (const unsigned char *)key; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) {
            return false;
        }
        if (++key_len > EF_ENV_NAME_MAX - prefix_len) {
            return false;
        }
    }

    memcpy(storage_key, prefix, prefix_len);
    memcpy(storage_key + prefix_len, key, key_len + 1);
    return true;
}

int mybot_kv_bk_env_get(const char *key, void *value, size_t capacity,
                        size_t *out_len) {
    char storage_key[EF_ENV_NAME_MAX + 1];
    const size_t buffer_size = sizeof(mybot_kv_record_t) + MYBOT_KV_VALUE_MAX + 1;
    int result = MYBOT_KV_BK_ENV_ERROR;

    if (!value || !out_len || !build_storage_key(key, storage_key)) {
        MYBOT_LOGE(TAG, "get failed: invalid request");
        return MYBOT_KV_BK_ENV_ERROR;
    }

    mybot_kv_record_t *record = psram_malloc(buffer_size);
    if (!record) {
        MYBOT_LOGE(TAG, "get failed: record allocation, bytes=%lu",
                (unsigned long)buffer_size);
        return MYBOT_KV_BK_ENV_ERROR;
    }

    int copied_len = bk_get_env_enhance(storage_key, record, (int)buffer_size);
    if (copied_len == 0) {
        result = MYBOT_KV_BK_ENV_NOT_FOUND;
    } else if (copied_len >= (int)sizeof(*record) &&
               record->magic == MYBOT_KV_RECORD_MAGIC &&
               record->version == MYBOT_KV_RECORD_VERSION &&
               record->header_size == sizeof(*record) &&
               record->value_size <= MYBOT_KV_VALUE_MAX && record->value_size <= capacity &&
               copied_len == (int)(sizeof(*record) + (size_t)record->value_size)) {
        if (record->value_size > 0) {
            memcpy(value, record->value, record->value_size);
        }
        *out_len = record->value_size;
        result = MYBOT_KV_BK_ENV_OK;
    }
    psram_free(record);

    if (result == MYBOT_KV_BK_ENV_ERROR) {
        MYBOT_LOGE(TAG, "get failed, key=%s", key);
    }
    return result;
}

int mybot_kv_bk_env_set(const char *key, const void *value, size_t len) {
    char storage_key[EF_ENV_NAME_MAX + 1];

    if ((!value && len != 0) || len > MYBOT_KV_VALUE_MAX ||
        !build_storage_key(key, storage_key)) {
        MYBOT_LOGE(TAG, "set failed: invalid request");
        return MYBOT_KV_BK_ENV_ERROR;
    }

    size_t record_size = sizeof(mybot_kv_record_t) + len;
    mybot_kv_record_t *record = psram_malloc(record_size);
    if (!record) {
        MYBOT_LOGE(TAG, "set failed: record allocation, bytes=%lu",
                (unsigned long)record_size);
        return MYBOT_KV_BK_ENV_ERROR;
    }

    record->magic = MYBOT_KV_RECORD_MAGIC;
    record->version = MYBOT_KV_RECORD_VERSION;
    record->header_size = sizeof(*record);
    record->value_size = (uint32_t)len;
    if (len > 0) {
        memcpy(record->value, value, len);
    }

    int result = bk_set_env_enhance(storage_key, record, (int)record_size) == EF_NO_ERR
                     ? MYBOT_KV_BK_ENV_OK
                     : MYBOT_KV_BK_ENV_ERROR;
    psram_free(record);
    if (result != MYBOT_KV_BK_ENV_OK) {
        MYBOT_LOGE(TAG, "set failed, key=%s bytes=%lu", key, (unsigned long)len);
    }
    return result;
}

int mybot_kv_bk_env_erase(const char *key) {
    char storage_key[EF_ENV_NAME_MAX + 1];

    if (!build_storage_key(key, storage_key)) {
        MYBOT_LOGE(TAG, "erase failed: invalid request");
        return MYBOT_KV_BK_ENV_ERROR;
    }

    EfErrCode error = bk_set_env_enhance(storage_key, NULL, 0);
    int result = error == EF_NO_ERR || error == EF_ENV_NAME_ERR
                     ? MYBOT_KV_BK_ENV_OK
                     : MYBOT_KV_BK_ENV_ERROR;
    if (result != MYBOT_KV_BK_ENV_OK) {
        MYBOT_LOGE(TAG, "erase failed, key=%s error=%d", key, (int)error);
    }
    return result;
}
