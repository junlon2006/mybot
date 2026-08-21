/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_wifi_runtime.h"

#include <common/bk_err.h>
#include <components/event.h>
#include "mybot_platform_log.h"
#include <components/netif_types.h>
#include <modules/wifi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIFI_RUNTIME_TAG "mybot_wifi"
#define WIFI_LOGW(format, ...) MYBOT_LOGW(WIFI_RUNTIME_TAG, format, ##__VA_ARGS__)
#define WIFI_LOGE(format, ...) MYBOT_LOGE(WIFI_RUNTIME_TAG, format, ##__VA_ARGS__)

bool mybot_wifi_string_is_terminated(const void *value, size_t capacity) {
    return value && memchr(value, '\0', capacity) != NULL;
}

bool mybot_wifi_runtime_lock(mybot_wifi_runtime_t *runtime) {
    return runtime && runtime->lock && rtos_lock_mutex(&runtime->lock) == BK_OK;
}

void mybot_wifi_runtime_unlock(mybot_wifi_runtime_t *runtime) {
    rtos_unlock_mutex(&runtime->lock);
}

int mybot_wifi_runtime_init(mybot_wifi_runtime_t *runtime) {
    if (!runtime) {
        return -1;
    }
    memset(runtime, 0, sizeof(*runtime));
    if (rtos_init_mutex(&runtime->lock) != BK_OK) {
        return -1;
    }
    if (rtos_init_semaphore(&runtime->event, 1) != BK_OK) {
        rtos_deinit_mutex(&runtime->lock);
        runtime->lock = NULL;
        return -1;
    }
    return 0;
}

void mybot_wifi_runtime_deinit(mybot_wifi_runtime_t *runtime) {
    if (!runtime) {
        return;
    }
    if (runtime->event) {
        rtos_deinit_semaphore(&runtime->event);
    }
    if (runtime->lock) {
        rtos_deinit_mutex(&runtime->lock);
    }
    memset(runtime, 0, sizeof(*runtime));
}

bool mybot_wifi_runtime_is_stopping(mybot_wifi_runtime_t *runtime) {
    bool stopping = true;
    if (mybot_wifi_runtime_lock(runtime)) {
        stopping = runtime->stopping;
        mybot_wifi_runtime_unlock(runtime);
    }
    return stopping;
}

void mybot_wifi_runtime_request_stop(mybot_wifi_runtime_t *runtime) {
    if (!runtime) {
        return;
    }
    if (!runtime->lock) {
        runtime->stopping = true;
    } else if (mybot_wifi_runtime_lock(runtime)) {
        runtime->stopping = true;
        mybot_wifi_runtime_unlock(runtime);
    } else {
        WIFI_LOGE("failed to lock runtime while stopping");
        return;
    }
    if (runtime->event) {
        rtos_set_semaphore(&runtime->event);
    }
}

uint32_t mybot_wifi_time_now(void) {
    return rtos_get_time();
}

bool mybot_wifi_time_reached(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

uint32_t mybot_wifi_time_remaining(uint32_t now, uint32_t deadline) {
    return mybot_wifi_time_reached(now, deadline) ? 0u : deadline - now;
}

void mybot_wifi_runtime_wait(mybot_wifi_runtime_t *runtime, uint32_t timeout_ms) {
    if (runtime && runtime->event) {
        rtos_get_semaphore(&runtime->event, timeout_ms);
    }
}

bool mybot_wifi_runtime_delay(mybot_wifi_runtime_t *runtime, uint32_t delay_ms) {
    uint32_t deadline = mybot_wifi_time_now() + delay_ms;
    while (!mybot_wifi_runtime_is_stopping(runtime)) {
        uint32_t remaining = mybot_wifi_time_remaining(mybot_wifi_time_now(), deadline);
        if (remaining == 0) {
            return true;
        }
        mybot_wifi_runtime_wait(runtime, remaining > 250 ? 250 : remaining);
    }
    return false;
}

static void set_events(mybot_wifi_runtime_t *runtime, uint32_t flags) {
    if (mybot_wifi_runtime_lock(runtime)) {
        runtime->event_flags |= flags;
        mybot_wifi_runtime_unlock(runtime);
    }
    if (runtime && runtime->event) {
        rtos_set_semaphore(&runtime->event);
    }
}

uint32_t mybot_wifi_runtime_take_events(mybot_wifi_runtime_t *runtime, uint32_t mask) {
    uint32_t flags = 0;
    if (mybot_wifi_runtime_lock(runtime)) {
        flags = runtime->event_flags & mask;
        runtime->event_flags &= ~mask;
        mybot_wifi_runtime_unlock(runtime);
    }
    return flags;
}

void mybot_wifi_runtime_drain_events(mybot_wifi_runtime_t *runtime, uint32_t mask) {
    (void)mybot_wifi_runtime_take_events(runtime, mask);
    if (runtime && runtime->event) {
        while (rtos_get_semaphore(&runtime->event, BEKEN_NO_WAIT) == BK_OK) {
        }
    }
}

static bk_err_t wifi_event_cb(void *arg, event_module_t module, int event_id,
                              void *event_data) {
    (void)module;
    (void)event_data;
    mybot_wifi_runtime_t *runtime = (mybot_wifi_runtime_t *)arg;
    if (mybot_wifi_runtime_is_stopping(runtime)) {
        return BK_OK;
    }

    switch (event_id) {
    case EVENT_WIFI_SCAN_DONE:
        set_events(runtime, MYBOT_WIFI_EVENT_SCAN_DONE);
        break;
    case EVENT_WIFI_STA_CONNECTED:
        set_events(runtime, MYBOT_WIFI_EVENT_STA_CONNECTED);
        break;
    case EVENT_WIFI_STA_DISCONNECTED:
        set_events(runtime, MYBOT_WIFI_EVENT_STA_DISCONNECTED);
        break;
    default:
        break;
    }
    return BK_OK;
}

static bk_err_t netif_event_cb(void *arg, event_module_t module, int event_id,
                               void *event_data) {
    (void)module;
    mybot_wifi_runtime_t *runtime = (mybot_wifi_runtime_t *)arg;
    if (mybot_wifi_runtime_is_stopping(runtime)) {
        return BK_OK;
    }

    if (event_id == EVENT_NETIF_GOT_IP4 && event_data) {
        netif_event_got_ip4_t *event = (netif_event_got_ip4_t *)event_data;
        if (event->netif_if == NETIF_IF_STA) {
            set_events(runtime, MYBOT_WIFI_EVENT_GOT_IP);
        }
    } else if (event_id == EVENT_NETIF_DHCP_TIMEOUT) {
        set_events(runtime, MYBOT_WIFI_EVENT_DHCP_TIMEOUT);
    }
    return BK_OK;
}

int mybot_wifi_runtime_register_callbacks(mybot_wifi_runtime_t *runtime) {
    if (!runtime || !runtime->lock || !runtime->event) {
        return -1;
    }
    bk_err_t result = bk_event_register_cb(EVENT_MOD_WIFI, EVENT_ID_ALL, wifi_event_cb, runtime);
    if (result != BK_OK && result != BK_ERR_EVENT_CB_EXIST) {
        WIFI_LOGE("Wi-Fi event registration failed: %d", result);
        return -1;
    }
    runtime->wifi_event_registered = true;

    result = bk_event_register_cb(EVENT_MOD_NETIF, EVENT_ID_ALL, netif_event_cb, runtime);
    if (result != BK_OK && result != BK_ERR_EVENT_CB_EXIST) {
        WIFI_LOGE("netif event registration failed: %d", result);
        (void)mybot_wifi_runtime_unregister_callbacks(runtime);
        return -1;
    }
    runtime->netif_event_registered = true;
    return 0;
}

bool mybot_wifi_runtime_unregister_callbacks(mybot_wifi_runtime_t *runtime) {
    if (!runtime) {
        return true;
    }
    bool detached = true;
    if (runtime->netif_event_registered) {
        bk_err_t result = bk_event_unregister_cb(EVENT_MOD_NETIF, EVENT_ID_ALL, netif_event_cb);
        if (result == BK_OK || result == BK_ERR_EVENT_NO_CB) {
            runtime->netif_event_registered = false;
        } else {
            WIFI_LOGE("failed to unregister netif events: %d", result);
            detached = false;
        }
    }
    if (runtime->wifi_event_registered) {
        bk_err_t result = bk_event_unregister_cb(EVENT_MOD_WIFI, EVENT_ID_ALL, wifi_event_cb);
        if (result == BK_OK || result == BK_ERR_EVENT_NO_CB) {
            runtime->wifi_event_registered = false;
        } else {
            WIFI_LOGE("failed to unregister Wi-Fi events: %d", result);
            detached = false;
        }
    }
    return detached;
}

bool mybot_wifi_runtime_stop_sta(mybot_wifi_runtime_t *runtime) {
    if (!runtime || !runtime->sta_owned) {
        return true;
    }
    if (bk_wifi_sta_stop() != BK_OK) {
        WIFI_LOGW("failed to stop STA");
        return false;
    }
    runtime->sta_owned = false;
    return true;
}

void mybot_wifi_runtime_set_sta_owned(mybot_wifi_runtime_t *runtime, bool owned) {
    if (runtime) {
        runtime->sta_owned = owned;
    }
}

int mybot_wifi_runtime_start_sta(mybot_wifi_runtime_t *runtime,
                                 const mybot_wifi_credential_entry_t *credential,
                                 uint8_t auto_reconnect_count) {
    if (!runtime || !credential || mybot_wifi_runtime_is_stopping(runtime) ||
        !mybot_wifi_runtime_stop_sta(runtime) || !mybot_wifi_runtime_delay(runtime, 100)) {
        return -1;
    }
    mybot_wifi_runtime_drain_events(runtime, MYBOT_WIFI_EVENT_LINK_MASK);
    if (mybot_wifi_runtime_is_stopping(runtime)) {
        return -1;
    }

    wifi_sta_config_t config = {0};
    snprintf(config.ssid, sizeof(config.ssid), "%s", credential->ssid);
    snprintf(config.password, sizeof(config.password), "%s", credential->password);
    config.security = WIFI_SECURITY_AUTO;
    config.auto_reconnect_count = auto_reconnect_count;
    config.auto_reconnect_timeout = 0;
    config.disable_auto_reconnect_after_disconnect = true;
    if (bk_wifi_sta_set_config(&config) != BK_OK || bk_wifi_sta_start() != BK_OK) {
        WIFI_LOGE("failed to start STA: ssid=%s", credential->ssid);
        return -1;
    }
    runtime->sta_owned = true;
    return 0;
}

bool mybot_wifi_runtime_sta_link_matches(const char *ssid) {
    wifi_linkstate_reason_t link_state = {0};
    wifi_link_status_t link_status = {0};
    return ssid && bk_wifi_sta_get_linkstate_with_reason(&link_state) == BK_OK &&
           link_state.state == WIFI_LINKSTATE_STA_GOT_IP &&
           bk_wifi_sta_get_link_status(&link_status) == BK_OK &&
           link_status.state == WIFI_LINKSTATE_STA_GOT_IP &&
           mybot_wifi_string_is_terminated(link_status.ssid, sizeof(link_status.ssid)) &&
           strcmp(link_status.ssid, ssid) == 0;
}

bool mybot_wifi_runtime_wait_for_ipv4(mybot_wifi_runtime_t *runtime, const char *ssid,
                                      uint32_t timeout_ms) {
    uint32_t deadline = mybot_wifi_time_now() + timeout_ms;
    while (!mybot_wifi_runtime_is_stopping(runtime)) {
        uint32_t flags =
            mybot_wifi_runtime_take_events(runtime, MYBOT_WIFI_EVENT_LINK_MASK);
        if (mybot_wifi_runtime_sta_link_matches(ssid)) {
            return true;
        }

        wifi_linkstate_reason_t state = {0};
        if ((flags & (MYBOT_WIFI_EVENT_STA_DISCONNECTED | MYBOT_WIFI_EVENT_DHCP_TIMEOUT)) != 0 &&
            bk_wifi_sta_get_linkstate_with_reason(&state) == BK_OK &&
            state.reason_code == WIFI_REASON_WRONG_PASSWORD) {
            return false;
        }

        uint32_t remaining = mybot_wifi_time_remaining(mybot_wifi_time_now(), deadline);
        if (remaining == 0) {
            return false;
        }
        mybot_wifi_runtime_wait(runtime, remaining > 250 ? 250 : remaining);
    }
    return false;
}

static int scan_result_compare(const void *left, const void *right) {
    const wifi_scan_ap_info_t *a = (const wifi_scan_ap_info_t *)left;
    const wifi_scan_ap_info_t *b = (const wifi_scan_ap_info_t *)right;
    return b->rssi - a->rssi;
}

int mybot_wifi_runtime_start_scan(mybot_wifi_runtime_t *runtime) {
    if (!runtime || mybot_wifi_runtime_is_stopping(runtime) || runtime->scan_in_progress ||
        bk_wifi_scan_start(NULL) != BK_OK) {
        return -1;
    }
    runtime->scan_in_progress = true;
    return 0;
}

bool mybot_wifi_runtime_stop_scan(mybot_wifi_runtime_t *runtime) {
    if (!runtime) {
        return true;
    }
    if (runtime->scan_in_progress && bk_wifi_scan_stop() != BK_OK) {
        WIFI_LOGW("failed to stop Wi-Fi scan");
        return false;
    }
    runtime->scan_in_progress = false;
    (void)mybot_wifi_runtime_take_events(runtime, MYBOT_WIFI_EVENT_SCAN_DONE);
    return true;
}

bool mybot_wifi_runtime_is_scan_in_progress(mybot_wifi_runtime_t *runtime) {
    return runtime && runtime->scan_in_progress;
}

int mybot_wifi_runtime_collect_scan_results(mybot_wifi_runtime_t *runtime,
                                            wifi_scan_ap_info_t *results, size_t capacity,
                                            size_t *result_count) {
    if (!runtime || !results || !result_count || capacity == 0) {
        return -1;
    }
    runtime->scan_in_progress = false;
    *result_count = 0;
    wifi_scan_result_t scan = {0};
    if (bk_wifi_scan_get_result(&scan) != BK_OK) {
        return -1;
    }

    for (int i = 0; i < scan.ap_num && *result_count < capacity; ++i) {
        if (!mybot_wifi_string_is_terminated(scan.aps[i].ssid, sizeof(scan.aps[i].ssid)) ||
            scan.aps[i].ssid[0] == '\0') {
            continue;
        }
        size_t duplicate = *result_count;
        for (size_t j = 0; j < *result_count; ++j) {
            if (strcmp(results[j].ssid, scan.aps[i].ssid) == 0) {
                duplicate = j;
                break;
            }
        }
        if (duplicate < *result_count) {
            if (scan.aps[i].rssi > results[duplicate].rssi) {
                results[duplicate] = scan.aps[i];
            }
        } else {
            results[(*result_count)++] = scan.aps[i];
        }
    }
    bk_wifi_scan_free_result(&scan);
    qsort(results, *result_count, sizeof(results[0]), scan_result_compare);
    return 0;
}

int mybot_wifi_runtime_scan_sync(mybot_wifi_runtime_t *runtime,
                                 wifi_scan_ap_info_t *results, size_t capacity,
                                 size_t *result_count, uint32_t timeout_ms) {
    (void)mybot_wifi_runtime_take_events(runtime, MYBOT_WIFI_EVENT_SCAN_DONE);
    if (mybot_wifi_runtime_start_scan(runtime) < 0) {
        return -1;
    }

    uint32_t deadline = mybot_wifi_time_now() + timeout_ms;
    while (!mybot_wifi_runtime_is_stopping(runtime)) {
        if ((mybot_wifi_runtime_take_events(runtime, MYBOT_WIFI_EVENT_SCAN_DONE) &
             MYBOT_WIFI_EVENT_SCAN_DONE) != 0) {
            return mybot_wifi_runtime_collect_scan_results(runtime, results, capacity,
                                                           result_count);
        }
        uint32_t remaining = mybot_wifi_time_remaining(mybot_wifi_time_now(), deadline);
        if (remaining == 0) {
            break;
        }
        mybot_wifi_runtime_wait(runtime, remaining > 250 ? 250 : remaining);
    }
    (void)mybot_wifi_runtime_stop_scan(runtime);
    return -1;
}
