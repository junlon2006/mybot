/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot_network.h>

#include "mybot_wifi_credentials.h"
#include "mybot_wifi_runtime.h"

#include <mybot_event.h>

#include "mybot_platform_log.h"
#include <modules/wifi.h>
#include <os/mem.h>
#include <os/os.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NETWORK_TAG "mybot_net"
#define NETWORK_LOGI(format, ...) MYBOT_LOGI(NETWORK_TAG, format, ##__VA_ARGS__)
#define NETWORK_LOGW(format, ...) MYBOT_LOGW(NETWORK_TAG, format, ##__VA_ARGS__)
#define NETWORK_LOGE(format, ...) MYBOT_LOGE(NETWORK_TAG, format, ##__VA_ARGS__)

#define NETWORK_SCAN_RESULT_CAPACITY 32
#define NETWORK_SCAN_TIMEOUT_MS 15000u
#define NETWORK_INITIAL_CONNECT_TIMEOUT_MS 60000u
#define NETWORK_CONNECT_TIMEOUT_MS 10000u
#define NETWORK_RECONNECT_RETRIES 5
#define NETWORK_RECONNECT_MIN_MS 10000u
#define NETWORK_RECONNECT_MAX_MS 300000u
#define NETWORK_WORKER_STACK_SIZE 6144
#define NETWORK_WORKER_PRIORITY 2

typedef struct {
    mybot_wifi_runtime_t wifi;
    bool connected;
    bool worker_started;
    uint32_t event_generation;
    beken_thread_t worker;
    mybot_wifi_credential_list_t credentials;
    wifi_scan_ap_info_t scan_results[NETWORK_SCAN_RESULT_CAPACITY];
    size_t scan_result_count;
} mybot_network_context_t;

/* The controller is the sole owner of start/stop and this published pointer. */
static mybot_network_context_t *s_network;

static bool exchange_connected(mybot_network_context_t *ctx, bool connected) {
    bool previous = false;
    if (mybot_wifi_runtime_lock(&ctx->wifi)) {
        previous = ctx->connected;
        ctx->connected = connected;
        mybot_wifi_runtime_unlock(&ctx->wifi);
    }
    return previous;
}

static void publish_connected(mybot_network_context_t *ctx, const char *ssid) {
    if (!mybot_wifi_runtime_is_stopping(&ctx->wifi) && !exchange_connected(ctx, true)) {
        NETWORK_LOGI("STA got IPv4: ssid=%s", ssid);
        if (mybot_event_post_with_generation(MYBOT_EVENT_NETWORK_CONNECTED,
                                             ctx->event_generation) != 0) {
            NETWORK_LOGE("failed to post connected event");
        }
    }
}

static void publish_disconnected(mybot_network_context_t *ctx, const char *ssid) {
    if (!mybot_wifi_runtime_is_stopping(&ctx->wifi) && exchange_connected(ctx, false)) {
        NETWORK_LOGW("STA disconnected: ssid=%s", ssid);
        if (mybot_event_post_with_generation(MYBOT_EVENT_NETWORK_DISCONNECTED,
                                             ctx->event_generation) != 0) {
            NETWORK_LOGE("failed to post disconnected event");
        }
    }
}

static void publish_failed(mybot_network_context_t *ctx) {
    if (!mybot_wifi_runtime_is_stopping(&ctx->wifi) &&
        mybot_event_post_with_generation(MYBOT_EVENT_NETWORK_FAILED,
                                         ctx->event_generation) != 0) {
        NETWORK_LOGE("failed to post connection-failed event");
    }
}

static int credential_rssi(const mybot_network_context_t *ctx, const char *ssid) {
    for (size_t i = 0; i < ctx->scan_result_count; ++i) {
        if (strcmp(ctx->scan_results[i].ssid, ssid) == 0) {
            return ctx->scan_results[i].rssi;
        }
    }
    return -128;
}

static int connect_saved_cycle(mybot_network_context_t *ctx, uint32_t deadline,
                               mybot_wifi_credential_entry_t *connected) {
    if (mybot_wifi_runtime_scan_sync(&ctx->wifi, ctx->scan_results,
                                     NETWORK_SCAN_RESULT_CAPACITY, &ctx->scan_result_count,
                                     NETWORK_SCAN_TIMEOUT_MS) < 0) {
        if (mybot_wifi_runtime_is_stopping(&ctx->wifi)) {
            return -1;
        }
        ctx->scan_result_count = 0;
    }

    uint8_t order[MYBOT_WIFI_MAX_CREDENTIALS];
    size_t order_count = 0;
    for (size_t i = 0; i < ctx->credentials.count; ++i) {
        if (credential_rssi(ctx, ctx->credentials.entries[i].ssid) > -128) {
            order[order_count++] = (uint8_t)i;
        }
    }
    for (size_t i = 0; i < order_count; ++i) {
        for (size_t j = i + 1; j < order_count; ++j) {
            if (credential_rssi(ctx, ctx->credentials.entries[order[j]].ssid) >
                credential_rssi(ctx, ctx->credentials.entries[order[i]].ssid)) {
                uint8_t swap = order[i];
                order[i] = order[j];
                order[j] = swap;
            }
        }
    }
    for (size_t i = 0; i < ctx->credentials.count; ++i) {
        if (credential_rssi(ctx, ctx->credentials.entries[i].ssid) == -128) {
            order[order_count++] = (uint8_t)i;
        }
    }

    for (size_t i = 0;
         i < order_count && !mybot_wifi_runtime_is_stopping(&ctx->wifi); ++i) {
        uint32_t remaining = mybot_wifi_time_remaining(mybot_wifi_time_now(), deadline);
        if (remaining == 0) {
            break;
        }
        mybot_wifi_credential_entry_t *candidate = &ctx->credentials.entries[order[i]];
        if (mybot_wifi_runtime_start_sta(&ctx->wifi, candidate, 1) == 0 &&
            mybot_wifi_runtime_wait_for_ipv4(
                &ctx->wifi, candidate->ssid,
                remaining < NETWORK_CONNECT_TIMEOUT_MS ? remaining
                                                       : NETWORK_CONNECT_TIMEOUT_MS)) {
            *connected = *candidate;
            return 0;
        }
        (void)mybot_wifi_runtime_stop_sta(&ctx->wifi);
    }
    return -1;
}

static int connect_saved_until(mybot_network_context_t *ctx, uint32_t timeout_ms,
                               mybot_wifi_credential_entry_t *connected) {
    uint32_t deadline = mybot_wifi_time_now() + timeout_ms;
    while (!mybot_wifi_runtime_is_stopping(&ctx->wifi) &&
           !mybot_wifi_time_reached(mybot_wifi_time_now(), deadline)) {
        if (connect_saved_cycle(ctx, deadline, connected) == 0) {
            return 0;
        }
    }
    (void)mybot_wifi_runtime_stop_sta(&ctx->wifi);
    return -1;
}

static bool reconnect_current(mybot_network_context_t *ctx,
                              const mybot_wifi_credential_entry_t *current) {
    for (int retry = 0;
         retry < NETWORK_RECONNECT_RETRIES && !mybot_wifi_runtime_is_stopping(&ctx->wifi);
         ++retry) {
        if (mybot_wifi_runtime_start_sta(&ctx->wifi, current, NETWORK_RECONNECT_RETRIES) == 0 &&
            mybot_wifi_runtime_wait_for_ipv4(&ctx->wifi, current->ssid,
                                             NETWORK_CONNECT_TIMEOUT_MS)) {
            return true;
        }
    }
    return false;
}

static bool find_existing_connection(mybot_network_context_t *ctx,
                                     mybot_wifi_credential_entry_t *connected) {
    wifi_link_status_t status = {0};
    if (bk_wifi_sta_get_link_status(&status) != BK_OK ||
        status.state != WIFI_LINKSTATE_STA_GOT_IP ||
        !mybot_wifi_string_is_terminated(status.ssid, sizeof(status.ssid))) {
        return false;
    }
    for (size_t i = 0; i < ctx->credentials.count; ++i) {
        if (strcmp(ctx->credentials.entries[i].ssid, status.ssid) == 0) {
            *connected = ctx->credentials.entries[i];
            mybot_wifi_runtime_set_sta_owned(&ctx->wifi, true);
            return true;
        }
    }
    return false;
}

static void network_worker(beken_thread_arg_t arg) {
    mybot_network_context_t *ctx = (mybot_network_context_t *)arg;
    mybot_wifi_credential_entry_t current = {0};
    uint32_t backoff_ms = NETWORK_RECONNECT_MIN_MS;
    bool connected = find_existing_connection(ctx, &current);
    bool failure_announced = false;

    NETWORK_LOGI("normal STA worker started: saved=%u", (unsigned)ctx->credentials.count);
    while (!mybot_wifi_runtime_is_stopping(&ctx->wifi)) {
        if (!connected) {
            connected = connect_saved_until(ctx, NETWORK_INITIAL_CONNECT_TIMEOUT_MS, &current) == 0;
            if (!connected) {
                if (!failure_announced) {
                    NETWORK_LOGW("saved-network connection failed");
                    publish_failed(ctx);
                    failure_announced = true;
                }
                (void)mybot_wifi_runtime_stop_sta(&ctx->wifi);
                if (!mybot_wifi_runtime_delay(&ctx->wifi, backoff_ms)) {
                    break;
                }
                backoff_ms = backoff_ms < NETWORK_RECONNECT_MAX_MS / 2
                                 ? backoff_ms * 2
                                 : NETWORK_RECONNECT_MAX_MS;
                continue;
            }
        }

        failure_announced = false;
        backoff_ms = NETWORK_RECONNECT_MIN_MS;
        publish_connected(ctx, current.ssid);
        while (!mybot_wifi_runtime_is_stopping(&ctx->wifi) && connected) {
            uint32_t flags =
                mybot_wifi_runtime_take_events(&ctx->wifi, MYBOT_WIFI_EVENT_LINK_MASK);
            bool link_matches = mybot_wifi_runtime_sta_link_matches(current.ssid);
            if ((flags & MYBOT_WIFI_EVENT_STA_DISCONNECTED) != 0 ||
                (flags & MYBOT_WIFI_EVENT_DHCP_TIMEOUT) != 0 || !link_matches) {
                if (link_matches) {
                    publish_connected(ctx, current.ssid);
                    continue;
                }
                connected = false;
                publish_disconnected(ctx, current.ssid);
                break;
            }
            if ((flags & MYBOT_WIFI_EVENT_GOT_IP) != 0) {
                publish_connected(ctx, current.ssid);
            }
            mybot_wifi_runtime_wait(&ctx->wifi, 1000);
        }

        if (mybot_wifi_runtime_is_stopping(&ctx->wifi)) {
            break;
        }
        connected = reconnect_current(ctx, &current);
        if (connected) {
            publish_connected(ctx, current.ssid);
        }
    }

    (void)mybot_wifi_runtime_stop_scan(&ctx->wifi);
    (void)mybot_wifi_runtime_stop_sta(&ctx->wifi);
    exchange_connected(ctx, false);
    NETWORK_LOGI("normal STA worker stopped");
    rtos_delete_thread(NULL);
}

static bool release_wifi(mybot_network_context_t *ctx) {
    bool released = mybot_wifi_runtime_stop_scan(&ctx->wifi);
    return mybot_wifi_runtime_stop_sta(&ctx->wifi) && released;
}

static void destroy_context(mybot_network_context_t *ctx) {
    mybot_wifi_runtime_deinit(&ctx->wifi);
    psram_free(ctx);
}

int mybot_network_is_configured(bool *configured) {
    if (!configured) {
        return -1;
    }
    mybot_wifi_credential_list_t *credentials = psram_zalloc(sizeof(*credentials));
    if (!credentials) {
        return -1;
    }
    int result = mybot_wifi_credentials_load(credentials);
    *configured = result == 0 && credentials->count > 0;
    psram_free(credentials);
    return result;
}

int mybot_network_start(uint32_t generation) {
    if (generation == 0 || s_network) {
        return -1;
    }
    mybot_network_context_t *ctx = psram_zalloc(sizeof(*ctx));
    if (!ctx) {
        return -1;
    }
    ctx->event_generation = generation;

    if (mybot_wifi_credentials_load(&ctx->credentials) < 0 || ctx->credentials.count == 0) {
        NETWORK_LOGE("normal STA start requires saved credentials");
        goto failed;
    }
    if (mybot_wifi_runtime_init(&ctx->wifi) < 0) {
        NETWORK_LOGE("failed to create Wi-Fi runtime");
        goto failed;
    }
    if (mybot_wifi_runtime_register_callbacks(&ctx->wifi) < 0) {
        goto failed;
    }
    if (rtos_create_psram_thread(&ctx->worker, NETWORK_WORKER_PRIORITY, "mybot_network",
                                 network_worker, NETWORK_WORKER_STACK_SIZE,
                                 (beken_thread_arg_t)ctx) != BK_OK) {
        NETWORK_LOGE("worker creation failed");
        goto failed;
    }
    ctx->worker_started = true;
    s_network = ctx;
    NETWORK_LOGI("normal STA started");
    return 0;

failed:
    mybot_wifi_runtime_request_stop(&ctx->wifi);
    bool callbacks_detached = mybot_wifi_runtime_unregister_callbacks(&ctx->wifi);
    if (ctx->worker_started) {
        rtos_thread_join(&ctx->worker);
        ctx->worker_started = false;
    }
    bool wifi_released = release_wifi(ctx);
    if (!wifi_released || !callbacks_detached) {
        s_network = ctx;
        NETWORK_LOGE("startup cleanup incomplete; retaining network context");
        return -1;
    }
    destroy_context(ctx);
    return -1;
}

int mybot_network_stop(void) {
    mybot_network_context_t *ctx = s_network;
    if (!ctx) {
        return 0;
    }

    NETWORK_LOGI("normal STA stop requested");
    mybot_wifi_runtime_request_stop(&ctx->wifi);
    bool callbacks_detached = mybot_wifi_runtime_unregister_callbacks(&ctx->wifi);
    if (ctx->worker_started) {
        rtos_thread_join(&ctx->worker);
        ctx->worker_started = false;
    }
    bool wifi_released = release_wifi(ctx);
    if (!wifi_released || !callbacks_detached) {
        NETWORK_LOGE("cleanup incomplete; retaining network context");
        return -1;
    }

    s_network = NULL;
    destroy_context(ctx);
    NETWORK_LOGI("normal STA stopped");
    return 0;
}

bool mybot_network_is_connected(void) {
    mybot_network_context_t *ctx = s_network;
    bool connected = false;
    if (ctx && mybot_wifi_runtime_lock(&ctx->wifi)) {
        connected = ctx->connected;
        mybot_wifi_runtime_unlock(&ctx->wifi);
    }
    return connected;
}
