/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_ringbuf.h"

#include <string.h>

/* AOSL cross-platform interfaces. */
#include <api/aosl_atomic.h>
#include <hal/aosl_hal_memory.h>

/* Keep one byte unfilled to distinguish full from empty. */
#define RINGBUF_GUARD_BYTE 1

typedef struct {
    int size;
    char *buf;
    aosl_atomic_t head; /* write position, published by the producer */
    aosl_atomic_t tail; /* read position, published by the consumer */
} ringbuf_internal_t;

static inline int data_size(ringbuf_internal_t *rb) {
    int head = (int)aosl_atomic_read(&rb->head);
    int tail = (int)aosl_atomic_read(&rb->tail);
    return (head + rb->size - tail) % rb->size;
}

static inline int free_size(ringbuf_internal_t *rb) {
    int head = (int)aosl_atomic_read(&rb->head);
    int tail = (int)aosl_atomic_read(&rb->tail);
    int used = (head + rb->size - tail) % rb->size;
    return rb->size - used - RINGBUF_GUARD_BYTE;
}

mybot_ringbuf_t mybot_ringbuf_create(int size) {
    if (size <= 0) {
        return NULL;
    }

    ringbuf_internal_t *rb = (ringbuf_internal_t *)aosl_hal_malloc(sizeof(ringbuf_internal_t));
    if (!rb) {
        return NULL;
    }

    rb->size = size + RINGBUF_GUARD_BYTE;
    rb->buf = (char *)aosl_hal_malloc(rb->size);
    if (!rb->buf) {
        aosl_hal_free(rb);
        return NULL;
    }
    aosl_atomic_set(&rb->head, 0);
    aosl_atomic_set(&rb->tail, 0);
    return (mybot_ringbuf_t)rb;
}

int mybot_ringbuf_destroy(mybot_ringbuf_t handle) {
    if (!handle) {
        return -1;
    }
    ringbuf_internal_t *rb = (ringbuf_internal_t *)handle;
    aosl_hal_free(rb->buf);
    aosl_hal_free(rb);
    return 0;
}

int mybot_ringbuf_clear(mybot_ringbuf_t handle) {
    if (!handle) {
        return -1;
    }
    ringbuf_internal_t *rb = (ringbuf_internal_t *)handle;
    aosl_atomic_set(&rb->head, 0);
    aosl_atomic_set(&rb->tail, 0);
    return 0;
}

int mybot_ringbuf_get_free_size(mybot_ringbuf_t handle) {
    if (!handle) {
        return -1;
    }
    return free_size((ringbuf_internal_t *)handle);
}

int mybot_ringbuf_get_data_size(mybot_ringbuf_t handle) {
    if (!handle) {
        return -1;
    }
    return data_size((ringbuf_internal_t *)handle);
}

int mybot_ringbuf_write(mybot_ringbuf_t handle, const char *src, int writelen) {
    if (!handle || !src || writelen <= 0) {
        return -1;
    }

    ringbuf_internal_t *rb = (ringbuf_internal_t *)handle;
    int head = (int)aosl_atomic_read(&rb->head);
    int tail = (int)aosl_atomic_read(&rb->tail);
    /* Acquire: the consumer publishes its completed reads through tail with a
     * release barrier; make sure we observe that before overwriting slots it
     * may still be reading (weak-memory platforms). */
    aosl_rmb();
    int used = (head + rb->size - tail) % rb->size;
    if (rb->size - used - RINGBUF_GUARD_BYTE < writelen) {
        return -1;
    }

    int pos = (head + writelen) % rb->size;
    if (pos >= head) {
        memcpy(rb->buf + head, src, writelen);
    } else {
        int remain = rb->size - head;
        memcpy(rb->buf + head, src, remain);
        memcpy(rb->buf, src + remain, writelen - remain);
    }
    /* Release: the payload writes must be visible before the head advances,
     * so the consumer never reads stale data from an already-published slot. */
    aosl_wmb();
    aosl_atomic_set(&rb->head, pos);
    return writelen;
}

int mybot_ringbuf_read(char *dst, int readlen, mybot_ringbuf_t handle) {
    if (!handle || !dst || readlen <= 0) {
        return -1;
    }

    ringbuf_internal_t *rb = (ringbuf_internal_t *)handle;
    int head = (int)aosl_atomic_read(&rb->head);
    /* Acquire: the producer publishes its payload writes before advancing
     * head; make sure we observe that before reading the slots (weak-memory
     * platforms). */
    aosl_rmb();
    int tail = (int)aosl_atomic_read(&rb->tail);
    int used = (head + rb->size - tail) % rb->size;
    if (used < readlen) {
        return -1;
    }

    int pos = (tail + readlen) % rb->size;
    if (pos >= tail) {
        memcpy(dst, rb->buf + tail, readlen);
    } else {
        int remain = rb->size - tail;
        memcpy(dst, rb->buf + tail, remain);
        memcpy(dst + remain, rb->buf, readlen - remain);
    }
    /* Release: the payload reads must complete before the tail advances, so
     * the producer never overwrites a slot we are still reading. */
    aosl_wmb();
    aosl_atomic_set(&rb->tail, pos);
    return readlen;
}
