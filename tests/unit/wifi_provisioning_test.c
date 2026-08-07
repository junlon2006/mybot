/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_wifi.h>

#include "api/aosl.h"
#include "api/aosl_atomic.h"
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
static aosl_atomic_t s_state_change_count;
static aosl_atomic_t s_last_state;

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

static void on_state_changed(mybot_wifi_provisioning_state_t state, void *user_data) {
    assert(user_data == &s_state_change_count);
    aosl_atomic_set(&s_last_state, state);
    aosl_atomic_inc(&s_state_change_count);
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
    assert(mybot_wifi_provisioning_init(NULL, on_state_changed, &s_state_change_count) < 0);

    s_initial_event = MYBOT_WIFI_PROVISIONING_EVENT_STA_CONNECTED;
    assert(mybot_wifi_provisioning_init("device-001", on_state_changed, &s_state_change_count) ==
           0);
    assert(s_init_count == 1);
    assert(mybot_wifi_provisioning_get_state() == MYBOT_WIFI_PROVISIONING_STATE_CONNECTED);
    assert(aosl_atomic_read(&s_state_change_count) == 1);
    assert(aosl_atomic_read(&s_last_state) == MYBOT_WIFI_PROVISIONING_STATE_CONNECTED);
    assert(mybot_wifi_provisioning_register(&fake_ops) < 0);

    s_fake.emit(MYBOT_WIFI_PROVISIONING_EVENT_STA_DISCONNECTED, s_fake.user_data);
    assert(mybot_wifi_provisioning_get_state() == MYBOT_WIFI_PROVISIONING_STATE_DISCONNECTED);
    s_fake.emit(MYBOT_WIFI_PROVISIONING_EVENT_STA_CONNECTED, s_fake.user_data);
    assert(mybot_wifi_provisioning_get_state() == MYBOT_WIFI_PROVISIONING_STATE_CONNECTED);

    mybot_wifi_provisioning_deinit();
    mybot_wifi_provisioning_deinit();
    assert(s_destroy_count == 1);
    assert(mybot_wifi_provisioning_get_state() == MYBOT_WIFI_PROVISIONING_STATE_IDLE);

    s_initial_event = MYBOT_WIFI_PROVISIONING_EVENT_FAILED;
    s_emit_async = true;
    intptr_t previous_state_changes = aosl_atomic_read(&s_state_change_count);
    assert(mybot_wifi_provisioning_init("device-001", on_state_changed, &s_state_change_count) ==
           0);
    assert(s_init_count == 2);
    assert(mybot_wifi_provisioning_get_state() == MYBOT_WIFI_PROVISIONING_STATE_PROVISIONING);

    for (int i = 0; i < 1000 && aosl_atomic_read(&s_state_change_count) == previous_state_changes;
         ++i) {
        aosl_hal_msleep(1);
    }
    assert(mybot_wifi_provisioning_get_state() == MYBOT_WIFI_PROVISIONING_STATE_FAILED);
    assert(aosl_atomic_read(&s_state_change_count) == previous_state_changes + 1);
    assert(aosl_atomic_read(&s_last_state) == MYBOT_WIFI_PROVISIONING_STATE_FAILED);

    mybot_wifi_provisioning_deinit();
    assert(s_destroy_count == 2);

    aosl_dtor();
    return 0;
}
