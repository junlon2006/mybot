#include "key_service/mybot_key_service.h"

#include <assert.h>
#include <stddef.h>

typedef struct {
    mybot_key_event_handler_t emit;
    void *user_data;
} fake_key_ctx_t;

static fake_key_ctx_t s_fake;
static int s_destroy_count;
static int s_handler_count;
static mybot_key_event_t s_last_event;

static int fake_init(void **ctx, mybot_key_event_handler_t emit, void *user_data) {
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
    const mybot_key_service_ops_t incomplete_ops = {0};
    const mybot_key_service_ops_t fake_ops = {
        .name = "fake",
        .init = fake_init,
        .destroy = fake_destroy,
    };

    assert(mybot_key_service_register(NULL) < 0);
    assert(mybot_key_service_register(&incomplete_ops) < 0);
    assert(mybot_key_service_register(&fake_ops) == 0);
    assert(mybot_key_service_init(NULL, NULL) < 0);
    assert(mybot_key_service_init(on_key, &s_handler_count) == 0);
    assert(mybot_key_service_register(&fake_ops) < 0);
    s_fake.emit(MYBOT_KEY_EVENT_PAIR, s_fake.user_data);
    assert(s_handler_count == 1);
    assert(s_last_event == MYBOT_KEY_EVENT_PAIR);

    mybot_key_service_deinit();
    mybot_key_service_deinit();
    assert(s_destroy_count == 1);
    assert(s_fake.emit == NULL);
    return 0;
}
