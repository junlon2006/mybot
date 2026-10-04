/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_key.h>

#include "mybot_key_internal.h"
#include "platform_test.h"

#include <assert.h>
#include <stddef.h>

typedef struct {
    mybot_key_event_handler_t emit;
    void *user_data;
} fake_key_ctx_t;

static fake_key_ctx_t s_fake;
static int s_destroy_count;
static int s_init_count;
static int s_init_result;
static int s_handler_count;
static mybot_key_event_t s_last_event;

static int fake_init(void **ctx, mybot_key_event_handler_t emit, void *user_data) {
    s_init_count++;
    if (s_init_result < 0) {
        *ctx = NULL;
        return s_init_result;
    }
    s_fake.emit = emit;
    s_fake.user_data = user_data;
    *ctx = &s_fake;
    return 0;
}

static void fake_destroy(void *ctx) {
    assert(ctx == &s_fake);
    s_fake.emit = NULL;
    s_fake.user_data = NULL;
    s_destroy_count++;
}

static void on_key(mybot_key_event_t event, void *user_data) {
    assert(user_data == &s_handler_count);
    s_last_event = event;
    s_handler_count++;
}

int main(void) {
    mybot_key_t key = {0};
    const mybot_key_ops_t fake_ops = {
        .init = fake_init,
        .destroy = fake_destroy,
    };

    assert(mybot_key_init(NULL, on_key, &s_handler_count) == -1);
    assert(mybot_key_init(&key, on_key, &s_handler_count) == -1);
    mybot_key_deinit(NULL);
    mybot_key_deinit(&key);
    assert(s_init_count == 0);
    assert(s_destroy_count == 0);

    mybot_platform_descriptor_t descriptor = mybot_test_platform_descriptor();
    descriptor.key = &fake_ops;
    assert(mybot_platform_register(&descriptor) == 0);
    assert(mybot_key_init(&key, NULL, NULL) < 0);
    s_init_result = -1;
    assert(mybot_key_init(&key, on_key, &s_handler_count) == -1);
    assert(!key.active);
    assert(key.ctx == NULL);
    mybot_key_deinit(&key);
    assert(s_destroy_count == 0);
    assert(s_fake.emit == NULL);
    s_init_result = 0;
    assert(mybot_key_init(&key, on_key, &s_handler_count) == 0);
    assert(s_init_count == 2);
    assert(mybot_key_init(&key, on_key, &s_handler_count) == -1);
    assert(s_init_count == 2);
    s_fake.emit(MYBOT_KEY_EVENT_PAIR, s_fake.user_data);
    assert(s_handler_count == 1);
    assert(s_last_event == MYBOT_KEY_EVENT_PAIR);
    s_fake.emit(MYBOT_KEY_EVENT_VOLUME_UP, s_fake.user_data);
    assert(s_handler_count == 2);
    assert(s_last_event == MYBOT_KEY_EVENT_VOLUME_UP);

    mybot_key_deinit(&key);
    mybot_key_deinit(&key);
    assert(s_destroy_count == 1);
    assert(s_fake.emit == NULL);
    assert(key.ctx == NULL);
    assert(!key.active);

    assert(mybot_key_init(&key, on_key, &s_handler_count) == 0);
    s_fake.emit(MYBOT_KEY_EVENT_EXIT, s_fake.user_data);
    assert(s_handler_count == 3);
    assert(s_last_event == MYBOT_KEY_EVENT_EXIT);
    mybot_key_deinit(&key);
    assert(s_destroy_count == 2);
    return 0;
}
