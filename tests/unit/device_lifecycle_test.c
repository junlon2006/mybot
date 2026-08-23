/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_device_lifecycle.h"
#include <mybot/platform/mybot_kv_store.h>

#include "mybot_kv_store_internal.h"

#include <api/aosl.h>

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static mybot_device_lifecycle_t s_lifecycle;
static mybot_kv_store_t s_kv_store;

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
static int s_pair_poll_after_seconds = 3;
static int s_start_result;
static int s_binding_poll_after_seconds = 1;
static bool s_start_missing_conversation_id;
static bool s_disconnect_during_start;
static int s_start_call_count;
static int s_renew_result;
static int s_renew_call_count;
static int s_sdk_renew_result;
static int s_sdk_renew_call_count;
static char s_last_renew_channel[128];
static char s_last_renew_uid[64];
static char s_last_renewed_token[MYBOT_DEVICE_CLIENT_MAX_TOKEN];
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
    resp->poll_after_seconds = s_pair_poll_after_seconds;
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
    resp->poll_after_seconds = s_binding_poll_after_seconds;
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
        mybot_device_lifecycle_set_network_available(&s_lifecycle, false);
        mybot_device_lifecycle_set_network_available(&s_lifecycle, true);
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

int mybot_device_client_renew_rtc_token(const char *base_url, const char *device_id,
                                        const char *device_token, const char *channel,
                                        const char *local_uid, mybot_device_rtc_token_t *resp) {
    assert(strcmp(base_url, "http://server") == 0);
    assert(strcmp(device_id, "device-1") == 0);
    assert(device_token != NULL);
    snprintf(s_last_renew_channel, sizeof(s_last_renew_channel), "%s", channel);
    snprintf(s_last_renew_uid, sizeof(s_last_renew_uid), "%s", local_uid);
    s_renew_call_count++;
    if (s_renew_result != 0) {
        return s_renew_result;
    }
    memset(resp, 0, sizeof(*resp));
    snprintf(resp->rtc_app_id, sizeof(resp->rtc_app_id), "%s", "rtc-app-id");
    snprintf(resp->rtc_channel, sizeof(resp->rtc_channel), "%s", channel);
    snprintf(resp->rtc_uid, sizeof(resp->rtc_uid), "%s", local_uid);
    snprintf(resp->rtc_token, sizeof(resp->rtc_token), "%s", "renewed-token");
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

static void on_conversation_start(const mybot_conversation_params_t *params, void *user_data) {
    assert(user_data == &s_lifecycle);
    assert(strcmp(params->conversation_id, "conversation-1") == 0);
    s_conversation_start_count++;
}

static void on_conversation_stop(void *user_data) {
    assert(user_data == &s_lifecycle);
    s_conversation_stop_count++;
}

static int on_rtc_token_renewed(const char *token, void *user_data) {
    assert(user_data == &s_lifecycle);
    snprintf(s_last_renewed_token, sizeof(s_last_renewed_token), "%s", token);
    s_sdk_renew_call_count++;
    return s_sdk_renew_result;
}

static void tick_many(int count) {
    for (int i = 0; i < count; i++) {
        mybot_device_lifecycle_tick(&s_lifecycle);
    }
}

static void begin_pairing(void) {
    assert(mybot_device_lifecycle_init(&s_lifecycle, &s_kv_store, "http://server", "device-1", NULL,
                                       NULL, NULL) == 0);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_UNPROVISIONED);
    mybot_device_lifecycle_tick(&s_lifecycle);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_AWAITING_CLAIM);
}

int main(void) {
    aosl_ctor();
    assert(mybot_kv_store_register(&s_mock_kv_store_ops) == 0);
    assert(mybot_kv_store_init(&s_kv_store) == 0);

    s_pair_result = -1;
    assert(mybot_device_lifecycle_init(&s_lifecycle, &s_kv_store, "http://server", "device-1", NULL,
                                       NULL, NULL) == 0);
    mybot_device_lifecycle_tick(&s_lifecycle);
    assert(s_pair_call_count == 1);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_UNPROVISIONED);

    s_pair_result = 0;
    tick_many(29);
    assert(s_pair_call_count == 1);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_UNPROVISIONED);
    mybot_device_lifecycle_tick(&s_lifecycle);
    assert(s_pair_call_count == 2);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_AWAITING_CLAIM);

    int pair_calls_before_failed = s_pair_call_count;
    strcpy(s_binding_status, "failed");
    tick_many(30);
    assert(s_pair_call_count == pair_calls_before_failed);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_AWAITING_CLAIM);
    mybot_device_lifecycle_tick(&s_lifecycle);
    assert(s_pair_call_count == pair_calls_before_failed + 1);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_AWAITING_CLAIM);

    /* Server hints below the supported range are clamped to three seconds. */
    s_pair_poll_after_seconds = 0;
    strcpy(s_binding_status, "pending");
    int binding_calls_before_min_poll = s_binding_call_count;
    begin_pairing();
    tick_many(29);
    assert(s_binding_call_count == binding_calls_before_min_poll);
    mybot_device_lifecycle_tick(&s_lifecycle);
    assert(s_binding_call_count == binding_calls_before_min_poll + 1);
    tick_many(29);
    assert(s_binding_call_count == binding_calls_before_min_poll + 1);
    mybot_device_lifecycle_tick(&s_lifecycle);
    assert(s_binding_call_count == binding_calls_before_min_poll + 2);
    s_pair_poll_after_seconds = 3;

    /* Large server hints are capped at one minute instead of overflowing the
     * 100 ms tick conversion or causing a request on every tick. */
    s_pair_poll_after_seconds = INT_MAX;
    int binding_calls_before_max_poll = s_binding_call_count;
    begin_pairing();
    tick_many(599);
    assert(s_binding_call_count == binding_calls_before_max_poll);
    mybot_device_lifecycle_tick(&s_lifecycle);
    assert(s_binding_call_count == binding_calls_before_max_poll + 1);
    s_pair_poll_after_seconds = 3;

    strcpy(s_binding_status, "bound");
    strcpy(s_binding_token, "device-token");
    begin_pairing();
    tick_many(30);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_RUNTIME);
    assert(strcmp(mybot_device_lifecycle_get_token(&s_lifecycle), "device-token") == 0);
    assert(s_kv_store_present);

    assert(mybot_device_lifecycle_init(&s_lifecycle, &s_kv_store, "http://server", "device-1", NULL,
                                       NULL, NULL) == 0);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_RUNTIME);
    assert(strcmp(mybot_device_lifecycle_get_token(&s_lifecycle), "device-token") == 0);

    s_binding_poll_after_seconds = INT_MAX;
    int runtime_binding_calls_before_max_poll = s_binding_call_count;
    tick_many(299);
    assert(s_binding_call_count == runtime_binding_calls_before_max_poll);
    mybot_device_lifecycle_tick(&s_lifecycle);
    assert(s_binding_call_count == runtime_binding_calls_before_max_poll + 1);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_RUNTIME);

    int runtime_binding_calls_after_max_poll = s_binding_call_count;
    tick_many(599);
    assert(s_binding_call_count == runtime_binding_calls_after_max_poll);
    mybot_device_lifecycle_tick(&s_lifecycle);
    assert(s_binding_call_count == runtime_binding_calls_after_max_poll + 1);
    s_binding_poll_after_seconds = 1;

    /* Reinitialization restores the default runtime interval for the next
     * scenario. */
    assert(mybot_device_lifecycle_init(&s_lifecycle, &s_kv_store, "http://server", "device-1", NULL,
                                       NULL, NULL) == 0);

    s_binding_result = 401;
    tick_many(300);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_UNPROVISIONED);
    assert(mybot_device_lifecycle_get_token(&s_lifecycle) == NULL);
    assert(!s_kv_store_present);

    s_binding_result = 0;
    s_binding_token[0] = '\0';
    begin_pairing();
    tick_many(30);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_AWAITING_CLAIM);
    assert(!s_kv_store_present);

    strcpy(s_binding_token, "one-time-token");
    s_kv_store_set_fails = true;
    tick_many(30);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_AWAITING_CLAIM);
    assert(!s_kv_store_present);

    s_binding_token[0] = '\0';
    s_kv_store_set_fails = false;
    tick_many(30);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_RUNTIME);
    assert(strcmp(mybot_device_lifecycle_get_token(&s_lifecycle), "one-time-token") == 0);
    assert(s_kv_store_present);

    mybot_device_lifecycle_callbacks_t callbacks = {
        .on_conversation_start = on_conversation_start,
        .on_conversation_stop = on_conversation_stop,
        .on_rtc_token_renewed = on_rtc_token_renewed,
        .user_data = &s_lifecycle,
    };
    assert(mybot_device_lifecycle_init(&s_lifecycle, &s_kv_store, "http://server", "device-1", NULL,
                                       NULL, &callbacks) == 0);

    s_start_missing_conversation_id = true;
    mybot_device_lifecycle_request_start(&s_lifecycle);
    mybot_device_lifecycle_tick(&s_lifecycle);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_RUNTIME);
    assert(s_conversation_start_count == 0);
    assert(s_start_call_count == 1);

    s_start_missing_conversation_id = false;
    mybot_device_lifecycle_request_start(&s_lifecycle);
    mybot_device_lifecycle_tick(&s_lifecycle);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_IN_CONVERSATION);
    assert(s_conversation_start_count == 1);
    assert(s_start_call_count == 2);

    s_renew_result = -1;
    mybot_device_lifecycle_request_rtc_token_renewal(&s_lifecycle);
    mybot_device_lifecycle_tick(&s_lifecycle);
    assert(s_renew_call_count == 1);
    assert(s_sdk_renew_call_count == 0);
    tick_many(9);
    assert(s_renew_call_count == 1);
    s_renew_result = 0;
    mybot_device_lifecycle_tick(&s_lifecycle);
    assert(s_renew_call_count == 2);
    assert(s_sdk_renew_call_count == 1);
    assert(strcmp(s_last_renew_channel, "rtc-channel") == 0);
    assert(strcmp(s_last_renew_uid, "rtc-uid") == 0);
    assert(strcmp(s_last_renewed_token, "renewed-token") == 0);

    s_sdk_renew_result = -1;
    mybot_device_lifecycle_request_rtc_token_renewal(&s_lifecycle);
    mybot_device_lifecycle_tick(&s_lifecycle);
    assert(s_renew_call_count == 3);
    assert(s_sdk_renew_call_count == 2);
    tick_many(9);
    s_sdk_renew_result = 0;
    mybot_device_lifecycle_tick(&s_lifecycle);
    assert(s_renew_call_count == 4);
    assert(s_sdk_renew_call_count == 3);

    mybot_device_lifecycle_shutdown(&s_lifecycle);
    assert(s_stop_call_count == 1);
    assert(strcmp(s_last_stop_reason, MYBOT_CONVERSATION_STOP_REASON_DEVICE_HANGUP) == 0);
    assert(s_conversation_stop_count == 1);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_RUNTIME);

    mybot_device_lifecycle_request_start(&s_lifecycle);
    mybot_device_lifecycle_tick(&s_lifecycle);
    assert(s_conversation_start_count == 1);

    /* A disconnect edge must survive an immediate reconnect and close RTC
     * locally without attempting an HTTP stop on the stale connection. */
    assert(mybot_device_lifecycle_init(&s_lifecycle, &s_kv_store, "http://server", "device-1", NULL,
                                       NULL, &callbacks) == 0);
    mybot_device_lifecycle_request_start(&s_lifecycle);
    mybot_device_lifecycle_tick(&s_lifecycle);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_IN_CONVERSATION);
    assert(s_conversation_start_count == 2);
    assert(s_start_call_count == 3);

    mybot_device_lifecycle_set_network_available(&s_lifecycle, false);
    mybot_device_lifecycle_set_network_available(&s_lifecycle, true);
    mybot_device_lifecycle_tick(&s_lifecycle);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_RUNTIME);
    assert(s_stop_call_count == 1);
    assert(s_conversation_stop_count == 2);

    /* No device-service request may run while offline, and an offline start
     * request must not be replayed after reconnect. */
    mybot_device_lifecycle_set_network_available(&s_lifecycle, false);
    mybot_device_lifecycle_tick(&s_lifecycle);
    int binding_calls_before = s_binding_call_count;
    tick_many(600);
    assert(s_binding_call_count == binding_calls_before);
    mybot_device_lifecycle_request_start(&s_lifecycle);
    mybot_device_lifecycle_set_network_available(&s_lifecycle, true);
    mybot_device_lifecycle_tick(&s_lifecycle);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_RUNTIME);
    assert(s_start_call_count == 3);

    /* A successful HTTP response from a connection that crossed a disconnect
     * edge is stale and must not start RTC after the network comes back. */
    s_disconnect_during_start = true;
    mybot_device_lifecycle_request_start(&s_lifecycle);
    mybot_device_lifecycle_tick(&s_lifecycle);
    s_disconnect_during_start = false;
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_RUNTIME);
    assert(s_conversation_start_count == 2);
    assert(s_start_call_count == 4);
    mybot_device_lifecycle_tick(&s_lifecycle);

    /* Shutdown after a disconnect must also clean up locally. */
    mybot_device_lifecycle_request_start(&s_lifecycle);
    mybot_device_lifecycle_tick(&s_lifecycle);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_IN_CONVERSATION);
    mybot_device_lifecycle_set_network_available(&s_lifecycle, false);
    mybot_device_lifecycle_shutdown(&s_lifecycle);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_RUNTIME);
    assert(s_stop_call_count == 1);
    assert(s_conversation_stop_count == 3);

    /* Binding status continues to be polled during a conversation. A bound
     * response keeps RTC active; an unbound response closes RTC locally and
     * clears the persisted credential before re-pairing. */
    assert(mybot_device_lifecycle_init(&s_lifecycle, &s_kv_store, "http://server", "device-1", NULL,
                                       NULL, &callbacks) == 0);
    strcpy(s_binding_status, "bound");
    s_binding_result = 0;
    mybot_device_lifecycle_request_start(&s_lifecycle);
    mybot_device_lifecycle_tick(&s_lifecycle);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_IN_CONVERSATION);

    int conversation_binding_calls = s_binding_call_count;
    int conversation_stop_callbacks = s_conversation_stop_count;
    tick_many(300);
    assert(s_binding_call_count == conversation_binding_calls + 1);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_IN_CONVERSATION);
    assert(s_conversation_stop_count == conversation_stop_callbacks);

    strcpy(s_binding_status, "unbound");
    tick_many(30);
    assert(s_binding_call_count == conversation_binding_calls + 2);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_UNPROVISIONED);
    assert(s_conversation_stop_count == conversation_stop_callbacks + 1);
    assert(s_stop_call_count == 1);
    assert(!s_kv_store_present);

    /* User-initiated re-pairing stops an active conversation with a reason
     * accepted by the device-service protocol. */
    strcpy(s_binding_status, "bound");
    strcpy(s_binding_token, "device-token");
    begin_pairing();
    tick_many(30);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_RUNTIME);
    mybot_device_lifecycle_request_start(&s_lifecycle);
    mybot_device_lifecycle_tick(&s_lifecycle);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_IN_CONVERSATION);

    mybot_device_lifecycle_request_pair(&s_lifecycle);
    mybot_device_lifecycle_tick(&s_lifecycle);
    assert(strcmp(s_last_stop_reason, MYBOT_CONVERSATION_STOP_REASON_USER_REQUESTED) == 0);
    assert(s_stop_call_count == 2);
    assert(mybot_device_lifecycle_get_state(&s_lifecycle) == MYBOT_DEVICE_STATE_AWAITING_CLAIM);

    /* Separate caller-owned contexts no longer share lifecycle state. */
    mybot_device_lifecycle_t first = {0};
    mybot_device_lifecycle_t second = {0};
    s_kv_store_present = false;
    s_pair_result = 0;
    assert(mybot_device_lifecycle_init(&first, &s_kv_store, "http://server", "device-a", NULL, NULL,
                                       NULL) == 0);
    assert(mybot_device_lifecycle_init(&second, &s_kv_store, "http://server", "device-b", NULL,
                                       NULL, NULL) == 0);
    mybot_device_lifecycle_tick(&first);
    assert(mybot_device_lifecycle_get_state(&first) == MYBOT_DEVICE_STATE_AWAITING_CLAIM);
    assert(mybot_device_lifecycle_get_state(&second) == MYBOT_DEVICE_STATE_UNPROVISIONED);

    mybot_kv_store_deinit(&s_kv_store);
    aosl_dtor();
    puts("device_lifecycle_test: ok");
    return 0;
}
