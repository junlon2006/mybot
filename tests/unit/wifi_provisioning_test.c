/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_wifi.h>

#include "mybot_wifi_internal.h"
#include "platform_test.h"

#include "api/aosl.h"
#include "api/aosl_atomic.h"
#include "hal/aosl_hal_time.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    mybot_wifi_event_handler_t emit;
    void *user_data;
} fake_wifi_ctx_t;

static fake_wifi_ctx_t s_fake;
static mybot_wifi_event_t s_initial_event;
static pthread_t s_emit_thread;
static bool s_emit_async;
static bool s_emit_thread_active;
static int s_init_count;
static int s_destroy_count;
static aosl_atomic_t s_event_count;
static aosl_atomic_t s_last_event;

static void *emit_initial_event(void *arg) {
    (void)arg;
    aosl_hal_msleep(10);
    s_fake.emit(s_initial_event, s_fake.user_data);
    return NULL;
}

static int fake_init(void **ctx, const char *device_id, mybot_wifi_event_handler_t emit,
                     void *user_data) {
    assert(strcmp(device_id, "device-001") == 0);
    s_fake.emit = emit;
    s_fake.user_data = user_data;
    *ctx = &s_fake;
    s_init_count++;
    if (s_emit_async) {
        int rc = pthread_create(&s_emit_thread, NULL, emit_initial_event, NULL);
        assert(rc == 0);
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

static void on_event(mybot_wifi_event_t event, void *user_data) {
    assert(user_data == &s_event_count);
    aosl_atomic_set(&s_last_event, event);
    aosl_atomic_inc(&s_event_count);
}

int main(void) {
    mybot_wifi_t wifi = {0};
    const mybot_wifi_ops_t fake_ops = {
        .name = "fake",
        .init = fake_init,
        .destroy = fake_destroy,
    };

    aosl_ctor();
    mybot_platform_descriptor_t descriptor = mybot_test_platform_descriptor();
    descriptor.wifi = &fake_ops;
    assert(mybot_platform_register(&descriptor) == 0);
    assert(mybot_wifi_init(&wifi, NULL, on_event, &s_event_count) < 0);
    assert(mybot_wifi_init(&wifi, "device-001", NULL, &s_event_count) < 0);

    s_initial_event = MYBOT_WIFI_EVENT_STA_CONNECTED;
    assert(mybot_wifi_init(&wifi, "device-001", on_event, &s_event_count) == 0);
    assert(s_init_count == 1);
    assert(aosl_atomic_read(&s_event_count) == 1);
    assert(aosl_atomic_read(&s_last_event) == MYBOT_WIFI_EVENT_STA_CONNECTED);

    s_fake.emit(MYBOT_WIFI_EVENT_STA_DISCONNECTED, s_fake.user_data);
    assert(aosl_atomic_read(&s_event_count) == 2);
    assert(aosl_atomic_read(&s_last_event) == MYBOT_WIFI_EVENT_STA_DISCONNECTED);
    s_fake.emit(MYBOT_WIFI_EVENT_STA_CONNECTED, s_fake.user_data);
    assert(aosl_atomic_read(&s_event_count) == 3);
    assert(aosl_atomic_read(&s_last_event) == MYBOT_WIFI_EVENT_STA_CONNECTED);

    mybot_wifi_deinit(&wifi);
    mybot_wifi_deinit(&wifi);
    assert(s_destroy_count == 1);

    s_initial_event = MYBOT_WIFI_EVENT_FAILED;
    s_emit_async = true;
    intptr_t previous_event_count = aosl_atomic_read(&s_event_count);
    assert(mybot_wifi_init(&wifi, "device-001", on_event, &s_event_count) == 0);
    assert(s_init_count == 2);

    for (int i = 0; i < 1000 && aosl_atomic_read(&s_event_count) == previous_event_count; ++i) {
        aosl_hal_msleep(1);
    }
    assert(aosl_atomic_read(&s_event_count) == previous_event_count + 1);
    assert(aosl_atomic_read(&s_last_event) == MYBOT_WIFI_EVENT_FAILED);

    mybot_wifi_deinit(&wifi);
    assert(s_destroy_count == 2);

    aosl_dtor();
    return 0;
}
