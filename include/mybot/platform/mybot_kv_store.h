/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_KV_STORE_H_
#define MYBOT_KV_STORE_H_

#include <stddef.h>
#include <mybot/mybot_errors.h>
#include <mybot/mybot_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Persistent key-value store operations.
 *
 * The implementation provides durable storage for small records (the SDK persists
 * device credentials through this interface). All callbacks are required.
 */
typedef struct {
    /** Implementation name for logging and diagnostics. */
    const char *name;

    /**
     * Allocate and open the store.
     *
     * @param ctx [out] store context handle
     * @return 0 on success, -1 on error
     */
    int (*init)(void **ctx);

    /**
     * Read the value stored under key.
     *
     * @param ctx      store context from init()
     * @param key      NUL-terminated key
     * @param value    [out] destination buffer
     * @param capacity size of value in bytes
     * @param out_len  [out] number of bytes written; set only on success
     * @return 0 on success, MYBOT_ERR_NOT_FOUND if the key is absent,
     *         -1 on error (including a value larger than capacity)
     */
    int (*get)(void *ctx, const char *key, void *value, size_t capacity, size_t *out_len);

    /**
     * Write value under key, replacing any existing entry.
     *
     * @param ctx   store context from init()
     * @param key   NUL-terminated key
     * @param value value bytes; may be NULL only when len is 0
     * @param len   value length in bytes
     * @return 0 on success, -1 on error
     *
     * @note Must survive power loss and must not expose a partially replaced
     *       record (atomic rename or equivalent).
     */
    int (*set)(void *ctx, const char *key, const void *value, size_t len);

    /**
     * Remove the entry stored under key.
     *
     * @param ctx store context from init()
     * @param key NUL-terminated key
     * @return 0 on success, -1 on error
     *
     * @note Must be idempotent.
     */
    int (*erase)(void *ctx, const char *key);

    /**
     * Close the store and release all resources.
     *
     * @param ctx store context from init()
     */
    void (*destroy)(void *ctx);
} mybot_kv_store_ops_t;

/**
 * Register the persistent KV-store implementation for the current platform.
 *
 * @param ops KV-store operations table; must remain valid for the process
 *            lifetime
 * @return 0 on success, -1 if ops is invalid or already registered
 *
 * @note Call exactly once, before mybot_start(). Protect stored device
 *       credentials with appropriate access control or encryption.
 */
MYBOT_API int mybot_kv_store_register(const mybot_kv_store_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_KV_STORE_H_ */
