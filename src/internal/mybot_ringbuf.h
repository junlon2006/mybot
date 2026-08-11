/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_RINGBUF_H_
#define MYBOT_RINGBUF_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Lock-free single-producer single-consumer ring buffer.
 *
 * Data access is thread-safe only in SPSC mode (one writer, one reader).
 * create(), destroy(), and clear() require external lifecycle synchronization
 * and must not overlap data access. Read, write, and size-query operations use
 * no locks and are non-blocking.
 *
 * Memory ordering: the payload is made visible to the peer before the index is
 * published (release) and the peer observes the published index before touching
 * the payload (acquire). This is guaranteed via AOSL barriers and holds on
 * weak-memory platforms.
 */

typedef void *mybot_ringbuf_t;

/** Create a ring buffer with given capacity (bytes). Returns NULL on failure. */
mybot_ringbuf_t mybot_ringbuf_create(int size);

/** Destroy the ring buffer. */
int mybot_ringbuf_destroy(mybot_ringbuf_t handle);

/** Reset to empty. */
int mybot_ringbuf_clear(mybot_ringbuf_t handle);

/** Number of free bytes available for writing. */
int mybot_ringbuf_get_free_size(mybot_ringbuf_t handle);

/** Number of bytes available for reading. */
int mybot_ringbuf_get_data_size(mybot_ringbuf_t handle);

/** Write data (non-blocking). Returns bytes written, or -1 on invalid input or
 *  insufficient space. */
int mybot_ringbuf_write(mybot_ringbuf_t handle, const char *src, int writelen);

/** Read data (non-blocking). Returns bytes read, or -1 on invalid input or
 *  insufficient data. */
int mybot_ringbuf_read(char *dst, int readlen, mybot_ringbuf_t handle);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_RINGBUF_H_ */
