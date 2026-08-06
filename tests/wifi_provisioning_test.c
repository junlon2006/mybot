#include "wifi/mybot_wifi_provisioning.h"

#include "api/aosl.h"
#include "hal/aosl_hal_time.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    mybot_wifi_provisioning_event_handler_t emit;
    void *user_data;
} fake_wifi_ctx_t;

static fake_wifi_ctx_t s_fake;
static mybot_wifi_provisioning_event_t s_initial_event;
static pthread_t s_emit_thread;
static bool s_emit_async;
static bool s_emit_thread_active;
static int s_init_count;
static int s_destroy_count;

static void *emit_initial_event(void *arg) {
    (void)arg;
    aosl_hal_msleep(10);
    s_fake.emit(s_initial_event, s_fake.user_data);
    return NULL;
}

static int fake_init(void **ctx, const char *device_id,
                     mybot_wifi_provisioning_event_handler_t emit, void *user_data) {
    assert(strcmp(device_id, "device-001") == 0);
    s_fake.emit = emit;
    s_fake.user_data = user_data;
    *ctx = &s_fake;
    s_init_count++;
    if (s_emit_async) {
        assert(pthread_create(&s_emit_thread, NULL, emit_initial_event, NULL) == 0);
        s_emit_thread_active = true;
    } else {
        emit(s_initial_event, user_data);
    }
    return 0;
}

static void fake_destroy(void *ctx) {
    assert(ctx == &s_fake);
    if (s_emit_thread_active) {
        assert(pthread_join(s_emit_thread, NULL) == 0);
        s_emit_thread_active = false;
    }
    s_fake.emit = NULL;
    s_fake.user_data = NULL;
    s_destroy_count++;
}

int main(void) {
    const mybot_wifi_provisioning_ops_t incomplete_ops = {0};
    const mybot_wifi_provisioning_ops_t fake_ops = {
        .name = "fake",
        .init = fake_init,
        .destroy = fake_destroy,
    };

    aosl_ctor();
    assert(mybot_wifi_provisioning_register(NULL) < 0);
    assert(mybot_wifi_provisioning_register(&incomplete_ops) < 0);
    assert(mybot_wifi_provisioning_register(&fake_ops) == 0);
    assert(mybot_wifi_provisioning_init(NULL) < 0);

    s_initial_event = MYBOT_WIFI_PROVISIONING_EVENT_STA_CONNECTED;
    assert(mybot_wifi_provisioning_init("device-001") == 0);
    assert(s_init_count == 1);
    assert(mybot_wifi_provisioning_is_connected());
    assert(mybot_wifi_provisioning_register(&fake_ops) < 0);

    s_fake.emit(MYBOT_WIFI_PROVISIONING_EVENT_STA_DISCONNECTED, s_fake.user_data);
    assert(!mybot_wifi_provisioning_is_connected());
    s_fake.emit(MYBOT_WIFI_PROVISIONING_EVENT_STA_CONNECTED, s_fake.user_data);
    assert(mybot_wifi_provisioning_is_connected());

    mybot_wifi_provisioning_deinit();
    mybot_wifi_provisioning_deinit();
    assert(s_destroy_count == 1);
    assert(!mybot_wifi_provisioning_is_connected());

    s_initial_event = MYBOT_WIFI_PROVISIONING_EVENT_FAILED;
    s_emit_async = true;
    assert(mybot_wifi_provisioning_init("device-001") < 0);
    assert(s_init_count == 2);
    assert(s_destroy_count == 2);
    assert(!mybot_wifi_provisioning_is_connected());

    aosl_dtor();
    return 0;
}
