/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_ringbuf.h"

#include <api/aosl_atomic.h>

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>

#define CAPACITY 8

static void expect_empty(mybot_ringbuf_t rb, int capacity) {
    assert(mybot_ringbuf_get_data_size(rb) == 0);
    assert(mybot_ringbuf_get_free_size(rb) == capacity);
    char byte = 0;
    assert(mybot_ringbuf_read(&byte, 1, rb) == -1);
}

static void test_create_destroy(void) {
    assert(mybot_ringbuf_create(0) == NULL);
    assert(mybot_ringbuf_create(-1) == NULL);
    assert(mybot_ringbuf_destroy(NULL) == -1);

    mybot_ringbuf_t rb = mybot_ringbuf_create(1);
    assert(rb != NULL);
    expect_empty(rb, 1);
    assert(mybot_ringbuf_destroy(rb) == 0);

    rb = mybot_ringbuf_create(4096);
    assert(rb != NULL);
    assert(mybot_ringbuf_destroy(rb) == 0);
}

static void test_full_and_empty(void) {
    mybot_ringbuf_t rb = mybot_ringbuf_create(CAPACITY);
    assert(rb != NULL);

    char payload[CAPACITY] = {0, 1, 2, 3, 4, 5, 6, 7};
    assert(mybot_ringbuf_write(rb, payload, CAPACITY) == CAPACITY);
    assert(mybot_ringbuf_get_free_size(rb) == 0);
    assert(mybot_ringbuf_get_data_size(rb) == CAPACITY);
    /* Exactly full: one more byte must be rejected. */
    assert(mybot_ringbuf_write(rb, payload, 1) == -1);

    char out[CAPACITY];
    assert(mybot_ringbuf_read(out, CAPACITY, rb) == CAPACITY);
    assert(memcmp(out, payload, CAPACITY) == 0);
    expect_empty(rb, CAPACITY);
    assert(mybot_ringbuf_read(out, 1, rb) == -1);

    mybot_ringbuf_destroy(rb);
}

static void test_wrap_around(void) {
    mybot_ringbuf_t rb = mybot_ringbuf_create(CAPACITY);
    assert(rb != NULL);

    /* write 6, read 4, write 5: both the write and the read cross the end of
     * the buffer, and the read covers two segments. */
    assert(mybot_ringbuf_write(rb, "abcdef", 6) == 6);
    char out[CAPACITY];
    assert(mybot_ringbuf_read(out, 4, rb) == 4);
    assert(memcmp(out, "abcd", 4) == 0);

    assert(mybot_ringbuf_write(rb, "ghijk", 5) == 5);
    assert(mybot_ringbuf_read(out, 5, rb) == 5);
    assert(memcmp(out, "efghi", 5) == 0);
    assert(mybot_ringbuf_read(out, 2, rb) == 2);
    assert(memcmp(out, "jk", 2) == 0);
    expect_empty(rb, CAPACITY);

    /* Fill exactly to capacity across the wrap boundary. */
    assert(mybot_ringbuf_write(rb, "abc", 3) == 3);
    assert(mybot_ringbuf_read(out, 1, rb) == 1);
    assert(out[0] == 'a');
    assert(mybot_ringbuf_write(rb, "defghi", 6) == 6);
    assert(mybot_ringbuf_get_free_size(rb) == 0);
    assert(mybot_ringbuf_read(out, 8, rb) == 8);
    assert(memcmp(out, "bcdefghi", 8) == 0);
    expect_empty(rb, CAPACITY);

    mybot_ringbuf_destroy(rb);
}

static void test_single_byte(void) {
    mybot_ringbuf_t rb = mybot_ringbuf_create(1);
    assert(rb != NULL);

    assert(mybot_ringbuf_write(rb, "x", 1) == 1);
    assert(mybot_ringbuf_write(rb, "y", 1) == -1);
    char out = 0;
    assert(mybot_ringbuf_read(&out, 1, rb) == 1);
    assert(out == 'x');
    expect_empty(rb, 1);

    mybot_ringbuf_destroy(rb);
}

static void test_clear_and_arguments(void) {
    mybot_ringbuf_t rb = mybot_ringbuf_create(16);
    assert(rb != NULL);

    assert(mybot_ringbuf_write(rb, "hello", 5) == 5);
    assert(mybot_ringbuf_clear(rb) == 0);
    expect_empty(rb, 16);
    assert(mybot_ringbuf_clear(NULL) == -1);

    assert(mybot_ringbuf_write(NULL, "x", 1) == -1);
    assert(mybot_ringbuf_write(rb, NULL, 1) == -1);
    assert(mybot_ringbuf_write(rb, "x", 0) == -1);
    assert(mybot_ringbuf_write(rb, "x", -1) == -1);
    assert(mybot_ringbuf_read(NULL, 1, rb) == -1);
    assert(mybot_ringbuf_read((char[]){0}, 0, rb) == -1);
    assert(mybot_ringbuf_read((char[]){0}, -1, rb) == -1);
    assert(mybot_ringbuf_get_free_size(NULL) == -1);
    assert(mybot_ringbuf_get_data_size(NULL) == -1);

    mybot_ringbuf_destroy(rb);
}

/* ----------------------------------------------------------
 * Single-producer / single-consumer concurrency
 * ---------------------------------------------------------- */
#define SPSC_CHUNK_SIZE 64
#define SPSC_CHUNKS 2000
#define SPSC_RING_SIZE 8192

typedef struct {
    mybot_ringbuf_t rb;
    aosl_atomic_t producer_done;
    aosl_atomic_t produced;
    aosl_atomic_t consumed;
    int failed;
} spsc_ctx_t;

static void *spsc_producer(void *arg) {
    spsc_ctx_t *ctx = (spsc_ctx_t *)arg;
    char chunk[SPSC_CHUNK_SIZE];
    for (uint32_t seq = 0; seq < SPSC_CHUNKS; seq++) {
        memset(chunk, (int)(seq * 7 + 1) & 0xff, sizeof(chunk));
        memcpy(chunk, &seq, sizeof(seq));
        while (mybot_ringbuf_write(ctx->rb, chunk, sizeof(chunk)) < 0) {
            sched_yield();
        }
        aosl_atomic_inc(&ctx->produced);
    }
    aosl_atomic_set(&ctx->producer_done, true);
    return NULL;
}

static void *spsc_consumer(void *arg) {
    spsc_ctx_t *ctx = (spsc_ctx_t *)arg;
    char chunk[SPSC_CHUNK_SIZE];
    uint32_t expected_seq = 0;
    for (;;) {
        if (mybot_ringbuf_read(chunk, sizeof(chunk), ctx->rb) == (int)sizeof(chunk)) {
            uint32_t seq = 0;
            memcpy(&seq, chunk, sizeof(seq));
            if (seq != expected_seq) {
                ctx->failed = 1;
                return NULL;
            }
            const unsigned char pattern = (unsigned char)(seq * 7 + 1);
            for (size_t i = sizeof(seq); i < sizeof(chunk); i++) {
                if ((unsigned char)chunk[i] != pattern) {
                    ctx->failed = 1;
                    return NULL;
                }
            }
            expected_seq++;
            aosl_atomic_inc(&ctx->consumed);
        } else if (aosl_atomic_read(&ctx->producer_done) &&
                   aosl_atomic_read(&ctx->produced) == aosl_atomic_read(&ctx->consumed)) {
            break;
        } else {
            sched_yield();
        }
    }
    return NULL;
}

static void test_spsc_concurrency(void) {
    spsc_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.rb = mybot_ringbuf_create(SPSC_RING_SIZE);
    assert(ctx.rb != NULL);

    pthread_t producer;
    pthread_t consumer;
    assert(pthread_create(&producer, NULL, spsc_producer, &ctx) == 0);
    assert(pthread_create(&consumer, NULL, spsc_consumer, &ctx) == 0);
    pthread_join(producer, NULL);
    pthread_join(consumer, NULL);

    assert(ctx.failed == 0);
    assert(aosl_atomic_read(&ctx.produced) == SPSC_CHUNKS);
    assert(aosl_atomic_read(&ctx.consumed) == SPSC_CHUNKS);
    assert(mybot_ringbuf_get_data_size(ctx.rb) == 0);

    mybot_ringbuf_destroy(ctx.rb);
}

int main(void) {
    test_create_destroy();
    test_full_and_empty();
    test_wrap_around();
    test_single_byte();
    test_clear_and_arguments();
    test_spsc_concurrency();
    return 0;
}
