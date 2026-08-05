#include "protocols/mybot_device_state.h"
#include "flash/mybot_flash_device.h"

#include <api/aosl.h>

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static unsigned char s_flash_data[2048];
static size_t s_flash_len;
static bool s_flash_present;
static bool s_flash_write_fails;

static int s_binding_result;
static char s_binding_status[16];
static char s_binding_token[MYBOT_DEVICE_API_MAX_TOKEN];
static int s_pair_result;
static int s_pair_call_count;
static int s_start_result;
static int s_stop_call_count;
static int s_conversation_start_count;
static int s_conversation_stop_count;

static int mock_flash_init(void **ctx) {
    *ctx = s_flash_data;
    return 0;
}

static int mock_flash_read(void *ctx, const char *key, void *data, size_t capacity,
                           size_t *out_len) {
    (void)ctx;
    (void)key;
    if (!s_flash_present) {
        return MYBOT_FLASH_NOT_FOUND;
    }
    if (s_flash_len > capacity) {
        return -1;
    }
    memcpy(data, s_flash_data, s_flash_len);
    *out_len = s_flash_len;
    return 0;
}

static int mock_flash_write(void *ctx, const char *key, const void *data, size_t len) {
    (void)ctx;
    (void)key;
    if (s_flash_write_fails || len > sizeof(s_flash_data)) {
        return -1;
    }
    memcpy(s_flash_data, data, len);
    s_flash_len = len;
    s_flash_present = true;
    return 0;
}

static int mock_flash_erase(void *ctx, const char *key) {
    (void)ctx;
    (void)key;
    s_flash_present = false;
    s_flash_len = 0;
    return 0;
}

static void mock_flash_destroy(void *ctx) {
    (void)ctx;
}

static const mybot_flash_ops_t s_mock_flash_ops = {
    .name = "mock",
    .init = mock_flash_init,
    .read = mock_flash_read,
    .write = mock_flash_write,
    .erase = mock_flash_erase,
    .destroy = mock_flash_destroy,
};

int mybot_device_api_create_pair_code(const char *base_url, const char *device_id,
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

int mybot_device_api_get_binding_status(const char *base_url, const char *device_id,
                                        const char *auth_header, mybot_device_binding_t *resp) {
    (void)base_url;
    (void)device_id;
    (void)auth_header;
    memset(resp, 0, sizeof(*resp));
    strcpy(resp->status, s_binding_status);
    strcpy(resp->device_token, s_binding_token);
    resp->poll_after_seconds = 1;
    return s_binding_result;
}

int mybot_device_api_start_conversation(const char *base_url, const char *device_id,
                                        const char *device_token, const char *body_params,
                                        mybot_device_conversation_t *resp) {
    (void)base_url;
    (void)device_id;
    (void)device_token;
    (void)body_params;
    if (s_start_result != 0) {
        return s_start_result;
    }
    memset(resp, 0, sizeof(*resp));
    strcpy(resp->conversation_id, "conversation-1");
    strcpy(resp->rtc_app_id, "rtc-app-id");
    strcpy(resp->rtc_channel, "rtc-channel");
    strcpy(resp->rtc_uid, "rtc-uid");
    strcpy(resp->rtc_token, "rtc-token");
    return 0;
}

int mybot_device_api_stop_conversation(const char *base_url, const char *device_id,
                                       const char *device_token, const char *conversation_id,
                                       const char *reason) {
    (void)base_url;
    (void)device_id;
    (void)device_token;
    assert(strcmp(conversation_id, "conversation-1") == 0);
    assert(strcmp(reason, "device_hangup") == 0);
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
        mybot_device_state_tick();
    }
}

static void begin_pairing(void) {
    assert(mybot_device_state_init("http://server", "device-1", NULL, NULL, NULL) == 0);
    assert(mybot_device_state_get() == MYBOT_DEVICE_STATE_UNPROVISIONED);
    mybot_device_state_tick();
    assert(mybot_device_state_get() == MYBOT_DEVICE_STATE_AWAITING_CLAIM);
}

int main(void) {
    aosl_ctor();
    assert(mybot_flash_register(&s_mock_flash_ops) == 0);
    assert(mybot_flash_init() == 0);

    s_pair_result = -1;
    assert(mybot_device_state_init("http://server", "device-1", NULL, NULL, NULL) == 0);
    mybot_device_state_tick();
    assert(s_pair_call_count == 1);
    assert(mybot_device_state_get() == MYBOT_DEVICE_STATE_UNPROVISIONED);

    s_pair_result = 0;
    tick_many(29);
    assert(s_pair_call_count == 1);
    assert(mybot_device_state_get() == MYBOT_DEVICE_STATE_UNPROVISIONED);
    mybot_device_state_tick();
    assert(s_pair_call_count == 2);
    assert(mybot_device_state_get() == MYBOT_DEVICE_STATE_AWAITING_CLAIM);

    strcpy(s_binding_status, "bound");
    strcpy(s_binding_token, "device-token");
    begin_pairing();
    tick_many(30);
    assert(mybot_device_state_get() == MYBOT_DEVICE_STATE_RUNTIME);
    assert(strcmp(mybot_device_state_get_token(), "device-token") == 0);
    assert(s_flash_present);

    assert(mybot_device_state_init("http://server", "device-1", NULL, NULL, NULL) == 0);
    assert(mybot_device_state_get() == MYBOT_DEVICE_STATE_RUNTIME);
    assert(strcmp(mybot_device_state_get_token(), "device-token") == 0);

    s_binding_result = 401;
    tick_many(300);
    assert(mybot_device_state_get() == MYBOT_DEVICE_STATE_UNPROVISIONED);
    assert(mybot_device_state_get_token() == NULL);
    assert(!s_flash_present);

    s_binding_result = 0;
    s_binding_token[0] = '\0';
    begin_pairing();
    tick_many(30);
    assert(mybot_device_state_get() == MYBOT_DEVICE_STATE_AWAITING_CLAIM);
    assert(!s_flash_present);

    strcpy(s_binding_token, "one-time-token");
    s_flash_write_fails = true;
    tick_many(30);
    assert(mybot_device_state_get() == MYBOT_DEVICE_STATE_AWAITING_CLAIM);
    assert(!s_flash_present);

    s_binding_token[0] = '\0';
    s_flash_write_fails = false;
    tick_many(30);
    assert(mybot_device_state_get() == MYBOT_DEVICE_STATE_RUNTIME);
    assert(strcmp(mybot_device_state_get_token(), "one-time-token") == 0);
    assert(s_flash_present);

    mybot_device_state_callbacks_t callbacks = {
        .on_conversation_start = on_conversation_start,
        .on_conversation_stop = on_conversation_stop,
    };
    assert(mybot_device_state_init("http://server", "device-1", NULL, NULL, &callbacks) == 0);
    mybot_device_state_request_start();
    mybot_device_state_tick();
    assert(mybot_device_state_get() == MYBOT_DEVICE_STATE_IN_CONVERSATION);
    assert(s_conversation_start_count == 1);

    mybot_device_state_shutdown();
    assert(s_stop_call_count == 1);
    assert(s_conversation_stop_count == 1);
    assert(mybot_device_state_get() == MYBOT_DEVICE_STATE_RUNTIME);

    mybot_device_state_request_start();
    mybot_device_state_tick();
    assert(s_conversation_start_count == 1);

    mybot_flash_deinit();
    aosl_dtor();
    puts("device_state_test: ok");
    return 0;
}
