#ifndef MYBOT_RINGBUF_H_
#define MYBOT_RINGBUF_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Lock-free single-producer single-consumer ring buffer.
 *
 * Thread-safe only when used in SPSC mode (one writer, one reader).
 * All operations are non-blocking (no locks).
 */

typedef void *ringbuf_t;

/** Create a ring buffer with given capacity (bytes). Returns NULL on failure. */
ringbuf_t ringbuf_create(int size);

/** Destroy the ring buffer. */
int ringbuf_destroy(ringbuf_t handle);

/** Reset to empty. */
int ringbuf_clear(ringbuf_t handle);

/** Number of free bytes available for writing. */
int ringbuf_get_free_size(ringbuf_t handle);

/** Number of bytes available for reading. */
int ringbuf_get_data_size(ringbuf_t handle);

/** Write data (non-blocking). Returns bytes written, or -1 if full. */
int ringbuf_write(ringbuf_t handle, const char *src, int writelen);

/** Read data (non-blocking). Returns bytes read, or -1 if empty. */
int ringbuf_read(char *dst, int readlen, ringbuf_t handle);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_RINGBUF_H_ */
