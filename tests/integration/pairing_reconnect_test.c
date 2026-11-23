/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/mybot.h>
#include <mybot/mybot_errors.h>

#include "platform_test.h"

#include <api/aosl_atomic.h>
#include <hal/aosl_hal_time.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static mybot_wifi_event_handler_t emit_wifi;
static void *wifi_owner;
static aosl_atomic_t pair_requests, code_displays, disconnect_displays;

static int wifi_init(void **ctx, const char *id, mybot_wifi_event_handler_t emit, void *owner) {
    emit_wifi = emit;
    wifi_owner = owner;
    return mybot_test_wifi_init(ctx, id, emit, owner);
}

static int empty_store_get(void *ctx, const char *key, void *value, size_t capacity, size_t *len) {
    (void)ctx;
    (void)key;
    (void)value;
    (void)capacity;
    *len = 0;
    return MYBOT_ERR_NOT_FOUND;
}

static int render(void *ctx, const mybot_lcd_content_t *content) {
    (void)ctx;
    assert(content->screen != MYBOT_LCD_SCREEN_READY && mybot_get_state() != MYBOT_STATE_READY);
    if (content->screen == MYBOT_LCD_SCREEN_PAIR_CODE) {
        assert(strcmp(content->pair_code, "804152") == 0);
        assert(mybot_get_state() == MYBOT_STATE_PAIRING);
        aosl_atomic_inc(&code_displays);
    } else if (content->screen == MYBOT_LCD_SCREEN_WIFI_DISCONNECTED) {
        assert(mybot_get_state() == MYBOT_STATE_WIFI_DISCONNECTED);
        aosl_atomic_inc(&disconnect_displays);
    }
    return 0;
}

/* Each simulated TLS connection owns its own read cursor. */
static int connect_service(void **connection, const char *host, uint16_t port, int timeout_ms) {
    (void)timeout_ms;
    assert(strcmp(host, "device.test") == 0 && port == 443);
    *connection = calloc(1, sizeof(const char *));
    return *connection ? 0 : -1;
}

static int send_request(void *connection, const void *data, size_t len, int timeout_ms) {
    (void)timeout_ms;
    const char **response = connection;
    const char pair_path[] = "POST /devices/pair-codes ";
    const char poll_path[] = "GET /devices/test-device/binding-status ";
    if (len >= sizeof(pair_path) - 1 && memcmp(data, pair_path, sizeof(pair_path) - 1) == 0) {
        *response =
            "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n"
            "{\"data\":{\"code\":\"804152\",\"pair_token\":\"test\",\"poll_after_seconds\":1}}";
        aosl_atomic_inc(&pair_requests);
    } else {
        assert(len >= sizeof(poll_path) - 1 && memcmp(data, poll_path, sizeof(poll_path) - 1) == 0);
        *response =
            "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n{\"data\":{\"status\":\"pending\"}}";
    }
    return (int)len;
}

static int read_response(void *connection, void *data, size_t capacity, int timeout_ms) {
    (void)timeout_ms;
    const char **cursor = connection;
    size_t len = strlen(*cursor);
    if (len > capacity) {
        len = capacity;
    }
    memcpy(data, *cursor, len);
    *cursor += len;
    return (int)len;
}

#if MYBOT_WAKE_WORDS
static int init_wake(void **ctx, int rate, int channels, int bits, mybot_wake_words_handler_t emit,
                     void *owner) {
    (void)emit;
    (void)owner;
    return mybot_test_audio_init(ctx, rate, channels, bits);
}

static int process_wake(void *ctx, const void *pcm, int frames) {
    (void)ctx;
    (void)pcm;
    (void)frames;
    return 0;
}
#endif

static void wait_for_display(aosl_atomic_t *counter, intptr_t expected) {
    for (int i = 0; i < 500 && aosl_atomic_read(counter) < expected; i++) {
        assert(mybot_get_state() != MYBOT_STATE_READY);
        aosl_hal_msleep(10);
    }
    assert(aosl_atomic_read(counter) >= expected);
}

int main(void) {
    static const mybot_wifi_ops_t wifi = {.init = wifi_init, .destroy = mybot_test_destroy};
    static const mybot_lcd_ops_t lcd = {
        .init = mybot_test_kv_init, .render = render, .destroy = mybot_test_destroy};
    static const mybot_https_ops_t https = {
        .connect = connect_service, .send = send_request, .recv = read_response, .close = free};
    static mybot_kv_store_ops_t store;
    mybot_platform_descriptor_t platform = mybot_test_platform_descriptor();
    store = *platform.kv_store;
    store.get = empty_store_get;
    platform.kv_store = &store;
    platform.wifi = &wifi;
    platform.lcd = &lcd;
    platform.https = &https;
#if MYBOT_WAKE_WORDS
    static const mybot_wake_words_ops_t wake = {
        .init = init_wake, .process = process_wake, .destroy = mybot_test_destroy};
    platform.wake_words = &wake;
#endif
    assert(mybot_platform_register(&platform) == 0);
    mybot_config_t config = {.server_base = "https://device.test", .device_id = "test-device"};
    assert(mybot_start(&config) == 0);
    emit_wifi(MYBOT_WIFI_EVENT_STA_CONNECTED, wifi_owner);
    wait_for_display(&code_displays, 1);
    emit_wifi(MYBOT_WIFI_EVENT_STA_DISCONNECTED, wifi_owner);
    wait_for_display(&disconnect_displays, 1);
    intptr_t before_reconnect = aosl_atomic_read(&code_displays);
    emit_wifi(MYBOT_WIFI_EVENT_STA_CONNECTED, wifi_owner);
    wait_for_display(&code_displays, before_reconnect + 1);
    assert(mybot_get_state() == MYBOT_STATE_PAIRING && aosl_atomic_read(&pair_requests) == 1);
    mybot_stop();
    assert(mybot_get_state() == MYBOT_STATE_STOPPED);
    return 0;
}
