#include "ringbuf.h"

#include <string.h>

/* AOSL HAL — cross-platform system interface */
#include <hal/aosl_hal_memory.h>

/* Keep one byte unfilled to distinguish full from empty */
#define RINGBUF_GUARD_BYTE 1

typedef struct {
    int   size;
    char *buf;
    int   head;  /* write position */
    int   tail;  /* read position */
} ringbuf_internal_t;

static inline int data_size(ringbuf_internal_t *rb)
{
    return (rb->head + rb->size - rb->tail) % rb->size;
}

static inline int free_size(ringbuf_internal_t *rb)
{
    return rb->size - data_size(rb) - RINGBUF_GUARD_BYTE;
}

mybot_ringbuf_t mybot_ringbuf_create(int size)
{
    if (size <= 0) {
        return NULL;
    }

    ringbuf_internal_t *rb = (ringbuf_internal_t *)aosl_hal_malloc(sizeof(ringbuf_internal_t));
    if (!rb) {
        return NULL;
    }

    rb->size = size + RINGBUF_GUARD_BYTE;
    rb->buf  = (char *)aosl_hal_malloc(rb->size);
    if (!rb->buf) {
        aosl_hal_free(rb);
        return NULL;
    }
    rb->head = rb->tail = 0;
    return (mybot_ringbuf_t)rb;
}

int mybot_ringbuf_destroy(mybot_ringbuf_t handle)
{
    if (!handle) {
        return -1;
    }
    ringbuf_internal_t *rb = (ringbuf_internal_t *)handle;
    aosl_hal_free(rb->buf);
    aosl_hal_free(rb);
    return 0;
}

int mybot_ringbuf_clear(mybot_ringbuf_t handle)
{
    if (!handle) {
        return -1;
    }
    ringbuf_internal_t *rb = (ringbuf_internal_t *)handle;
    rb->head = rb->tail = 0;
    return 0;
}

int mybot_ringbuf_get_free_size(mybot_ringbuf_t handle)
{
    if (!handle) {
        return -1;
    }
    return free_size((ringbuf_internal_t *)handle);
}

int mybot_ringbuf_get_data_size(mybot_ringbuf_t handle)
{
    if (!handle) {
        return -1;
    }
    return data_size((ringbuf_internal_t *)handle);
}

int mybot_ringbuf_write(mybot_ringbuf_t handle, const char *src, int writelen)
{
    if (!handle || !src || writelen <= 0) {
        return -1;
    }

    ringbuf_internal_t *rb = (ringbuf_internal_t *)handle;
    if (free_size(rb) < writelen) {
        return -1;
    }

    int pos = (rb->head + writelen) % rb->size;
    if (pos >= rb->head) {
        memcpy(rb->buf + rb->head, src, writelen);
    } else {
        int remain = rb->size - rb->head;
        memcpy(rb->buf + rb->head, src, remain);
        memcpy(rb->buf, src + remain, writelen - remain);
    }
    rb->head = pos;
    return writelen;
}

int mybot_ringbuf_read(char *dst, int readlen, mybot_ringbuf_t handle)
{
    if (!handle || !dst || readlen <= 0) {
        return -1;
    }

    ringbuf_internal_t *rb = (ringbuf_internal_t *)handle;
    if (data_size(rb) < readlen) {
        return -1;
    }

    int pos = (rb->tail + readlen) % rb->size;
    if (pos >= rb->tail) {
        memcpy(dst, rb->buf + rb->tail, readlen);
    } else {
        int remain = rb->size - rb->tail;
        memcpy(dst, rb->buf + rb->tail, remain);
        memcpy(dst + remain, rb->buf, readlen - remain);
    }
    rb->tail = pos;
    return readlen;
}
