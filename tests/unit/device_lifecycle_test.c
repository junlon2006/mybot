/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_device_lifecycle.h"
#include <mybot/platform/mybot_kv_store.h>

#include "mybot_kv_store_internal.h"

#include <api/aosl.h>

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static unsigned char s_kv_store_data[2048];
static size_t s_kv_store_len;
static bool s_kv_store_present;
static bool s_kv_store_set_fails;

static int s_binding_result;
static int s_binding_call_count;
static char s_binding_status[16];
static char s_binding_token[MYBOT_DEVICE_CLIENT_MAX_TOKEN];
static int s_pair_result;
static int s_pair_call_count;
static int s_start_result;
static bool s_start_missing_conversation_id;
static bool s_disconnect_during_start;
static int s_start_call_count;
static int s_stop_call_count;
static char s_last_stop_reason[32];
static int s_conversation_start_count;
static int s_conversation_stop_count;

static int mock_kv_store_init(void **ctx) {
    *ctx = s_kv_store_data;
    return 0;
}

static int mock_kv_store_get(void *ctx, const char *key, void *value, size_t capacity,
                             size_t *out_len) {
    (void)ctx;
    (void)key;
    if (!s_kv_store_present) {
        return MYBOT_ERR_NOT_FOUND;
    }
    if (s_kv_store_len > capacity) {
        return -1;
    }
    memcpy(value, s_kv_store_data, s_kv_store_len);
    *out_len = s_kv_store_len;
    return 0;
}

static int mock_kv_store_set(void *ctx, const char *key, const void *value, size_t len) {
    (void)ctx;
    (void)key;
    if (s_kv_store_set_fails || len > sizeof(s_kv_store_data)) {
        return -1;
    }
    memcpy(s_kv_store_data, value, len);
    s_kv_store_len = len;
    s_kv_store_present = true;
    return 0;
}

static int mock_kv_store_erase(void *ctx, const char *key) {
    (void)ctx;
    (void)key;
    s_kv_store_present = false;
    s_kv_store_len = 0;
    return 0;
}

static void mock_kv_store_destroy(void *ctx) {
    (void)ctx;
}

static const mybot_kv_store_ops_t s_mock_kv_store_ops = {
    .name = "mock",
    .init = mock_kv_store_init,
    .get = mock_kv_store_get,
    .set = mock_kv_store_set,
    .erase = mock_kv_store_erase,
    .destroy = mock_kv_store_destroy,
};

int mybot_device_client_create_pair_code(const char *base_url, const char *device_id,
                                         const char *firmware_ver, const char *hw_model,
                                         mybot_device_pair_code_t *resp) {
    (void)base_url;
    (void)device_id;
    (void)firmware_ver;
    (void)hw_model;
    s_pair_call_count++;
    if (s_pair_result != 0) {
        return s_pair_result;
    }
    memset(resp, 0, sizeof(*resp));
    strcpy(resp->code, "123456");
    strcpy(resp->pair_token, "pair-token");
    resp->poll_after_seconds = 3;
    return 0;
}

int mybot_device_client_get_binding_status(const char *base_url, const char *device_id,
                                           const char *auth_header, mybot_device_binding_t *resp) {
    (void)base_url;
    (void)device_id;
    (void)auth_header;
    s_binding_call_count++;
    memset(resp, 0, sizeof(*resp));
    snprintf(resp->status, sizeof(resp->status), "%s", s_binding_status);
    snprintf(resp->device_token, sizeof(resp->device_token), "%s", s_binding_token);
    resp->poll_after_seconds = 1;
    return s_binding_result;
}

int mybot_device_client_start_conversation(const char *base_url, const char *device_id,
                                           const char *device_token, const char *body_params,
                                           mybot_device_conversation_t *resp) {
    (void)base_url;
    (void)device_id;
    (void)device_token;
    (void)body_params;
    s_start_call_count++;
    if (s_disconnect_during_start) {
        mybot_device_lifecycle_set_network_available(false);
        mybot_device_lifecycle_set_network_available(true);
    }
    if (s_start_result != 0) {
        return s_start_result;
    }
    memset(resp, 0, sizeof(*resp));
    if (!s_start_missing_conversation_id) {
        strcpy(resp->conversation_id, "conversation-1");
    }
    strcpy(resp->rtc_app_id, "rtc-app-id");
    strcpy(resp->rtc_channel, "rtc-channel");
    strcpy(resp->rtc_uid, "rtc-uid");
    strcpy(resp->rtc_token, "rtc-token");
    return 0;
}

int mybot_device_client_stop_conversation(const char *base_url, const char *device_id,
                                          const char *device_token, const char *conversation_id,
                                          const char *reason) {
    (void)base_url;
    (void)device_id;
    (void)device_token;
    assert(strcmp(conversation_id, "conversation-1") == 0);
    snprintf(s_last_stop_reason, sizeof(s_last_stop_reason), "%s", reason);
    s_stop_call_count++;
    return 0;
}

static void on_conversation_start(const mybot_conversation_params_t *params) {
    assert(strcmp(params->conversation_id, "conversation-1") == 0);
    s_conversation_start_count++;
}

static void on_conversation_stop(void) {
    s_conversation_stop_count++;
}

static void tick_many(int count) {
    for (int i = 0; i < count; i++) {
        mybot_device_lifecycle_tick();
    }
}

static void begin_pairing(void) {
    assert(mybot_device_lifecycle_init("http://server", "device-1", NULL, NULL, NULL) == 0);
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_UNPROVISIONED);
    mybot_device_lifecycle_tick();
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_AWAITING_CLAIM);
}

int main(void) {
    aosl_ctor();
    assert(mybot_kv_store_register(&s_mock_kv_store_ops) == 0);
    assert(mybot_kv_store_init() == 0);

    s_pair_result = -1;
    assert(mybot_device_lifecycle_init("http://server", "device-1", NULL, NULL, NULL) == 0);
    mybot_device_lifecycle_tick();
    assert(s_pair_call_count == 1);
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_UNPROVISIONED);

    s_pair_result = 0;
    tick_many(29);
    assert(s_pair_call_count == 1);
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_UNPROVISIONED);
    mybot_device_lifecycle_tick();
    assert(s_pair_call_count == 2);
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_AWAITING_CLAIM);

    strcpy(s_binding_status, "bound");
    strcpy(s_binding_token, "device-token");
    begin_pairing();
    tick_many(30);
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_RUNTIME);
    assert(strcmp(mybot_device_lifecycle_get_token(), "device-token") == 0);
    assert(s_kv_store_present);

    assert(mybot_device_lifecycle_init("http://server", "device-1", NULL, NULL, NULL) == 0);
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_RUNTIME);
    assert(strcmp(mybot_device_lifecycle_get_token(), "device-token") == 0);

    s_binding_result = 401;
    tick_many(300);
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_UNPROVISIONED);
    assert(mybot_device_lifecycle_get_token() == NULL);
    assert(!s_kv_store_present);

    s_binding_result = 0;
    s_binding_token[0] = '\0';
    begin_pairing();
    tick_many(30);
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_AWAITING_CLAIM);
    assert(!s_kv_store_present);

    strcpy(s_binding_token, "one-time-token");
    s_kv_store_set_fails = true;
    tick_many(30);
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_AWAITING_CLAIM);
    assert(!s_kv_store_present);

    s_binding_token[0] = '\0';
    s_kv_store_set_fails = false;
    tick_many(30);
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_RUNTIME);
    assert(strcmp(mybot_device_lifecycle_get_token(), "one-time-token") == 0);
    assert(s_kv_store_present);

    mybot_device_lifecycle_callbacks_t callbacks = {
        .on_conversation_start = on_conversation_start,
        .on_conversation_stop = on_conversation_stop,
    };
    assert(mybot_device_lifecycle_init("http://server", "device-1", NULL, NULL, &callbacks) == 0);

    s_start_missing_conversation_id = true;
    mybot_device_lifecycle_request_start();
    mybot_device_lifecycle_tick();
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_RUNTIME);
    assert(s_conversation_start_count == 0);
    assert(s_start_call_count == 1);

    s_start_missing_conversation_id = false;
    mybot_device_lifecycle_request_start();
    mybot_device_lifecycle_tick();
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_IN_CONVERSATION);
    assert(s_conversation_start_count == 1);
    assert(s_start_call_count == 2);

    mybot_device_lifecycle_shutdown();
    assert(s_stop_call_count == 1);
    assert(strcmp(s_last_stop_reason, MYBOT_CONVERSATION_STOP_REASON_DEVICE_HANGUP) == 0);
    assert(s_conversation_stop_count == 1);
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_RUNTIME);

    mybot_device_lifecycle_request_start();
    mybot_device_lifecycle_tick();
    assert(s_conversation_start_count == 1);

    /* A disconnect edge must survive an immediate reconnect and close RTC
     * locally without attempting an HTTP stop on the stale connection. */
    assert(mybot_device_lifecycle_init("http://server", "device-1", NULL, NULL, &callbacks) == 0);
    mybot_device_lifecycle_request_start();
    mybot_device_lifecycle_tick();
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_IN_CONVERSATION);
    assert(s_conversation_start_count == 2);
    assert(s_start_call_count == 3);

    mybot_device_lifecycle_set_network_available(false);
    mybot_device_lifecycle_set_network_available(true);
    mybot_device_lifecycle_tick();
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_RUNTIME);
    assert(s_stop_call_count == 1);
    assert(s_conversation_stop_count == 2);

    /* No device-service request may run while offline, and an offline start
     * request must not be replayed after reconnect. */
    mybot_device_lifecycle_set_network_available(false);
    mybot_device_lifecycle_tick();
    int binding_calls_before = s_binding_call_count;
    tick_many(600);
    assert(s_binding_call_count == binding_calls_before);
    mybot_device_lifecycle_request_start();
    mybot_device_lifecycle_set_network_available(true);
    mybot_device_lifecycle_tick();
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_RUNTIME);
    assert(s_start_call_count == 3);

    /* A successful HTTP response from a connection that crossed a disconnect
     * edge is stale and must not start RTC after the network comes back. */
    s_disconnect_during_start = true;
    mybot_device_lifecycle_request_start();
    mybot_device_lifecycle_tick();
    s_disconnect_during_start = false;
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_RUNTIME);
    assert(s_conversation_start_count == 2);
    assert(s_start_call_count == 4);
    mybot_device_lifecycle_tick();

    /* Shutdown after a disconnect must also clean up locally. */
    mybot_device_lifecycle_request_start();
    mybot_device_lifecycle_tick();
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_IN_CONVERSATION);
    mybot_device_lifecycle_set_network_available(false);
    mybot_device_lifecycle_shutdown();
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_RUNTIME);
    assert(s_stop_call_count == 1);
    assert(s_conversation_stop_count == 3);

    /* Binding status continues to be polled during a conversation. A bound
     * response keeps RTC active; an unbound response closes RTC locally and
     * clears the persisted credential before re-pairing. */
    assert(mybot_device_lifecycle_init("http://server", "device-1", NULL, NULL, &callbacks) == 0);
    strcpy(s_binding_status, "bound");
    s_binding_result = 0;
    mybot_device_lifecycle_request_start();
    mybot_device_lifecycle_tick();
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_IN_CONVERSATION);

    int conversation_binding_calls = s_binding_call_count;
    int conversation_stop_callbacks = s_conversation_stop_count;
    tick_many(300);
    assert(s_binding_call_count == conversation_binding_calls + 1);
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_IN_CONVERSATION);
    assert(s_conversation_stop_count == conversation_stop_callbacks);

    strcpy(s_binding_status, "unbound");
    tick_many(10);
    assert(s_binding_call_count == conversation_binding_calls + 2);
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_UNPROVISIONED);
    assert(s_conversation_stop_count == conversation_stop_callbacks + 1);
    assert(s_stop_call_count == 1);
    assert(!s_kv_store_present);

    /* User-initiated re-pairing stops an active conversation with a reason
     * accepted by the device-service protocol. */
    strcpy(s_binding_status, "bound");
    strcpy(s_binding_token, "device-token");
    begin_pairing();
    tick_many(30);
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_RUNTIME);
    mybot_device_lifecycle_request_start();
    mybot_device_lifecycle_tick();
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_IN_CONVERSATION);

    mybot_device_lifecycle_request_pair();
    mybot_device_lifecycle_tick();
    assert(strcmp(s_last_stop_reason, MYBOT_CONVERSATION_STOP_REASON_USER_REQUESTED) == 0);
    assert(s_stop_call_count == 2);
    assert(mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_AWAITING_CLAIM);

    mybot_kv_store_deinit();
    aosl_dtor();
    puts("device_lifecycle_test: ok");
    return 0;
}
