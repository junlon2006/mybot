/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_WIFI_RUNTIME_H_
#define MYBOT_WIFI_RUNTIME_H_

#include "mybot_wifi_credentials.h"

#include <modules/wifi_types.h>
#include <os/os.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MYBOT_WIFI_EVENT_SCAN_DONE (1u << 0)
#define MYBOT_WIFI_EVENT_STA_CONNECTED (1u << 1)
#define MYBOT_WIFI_EVENT_STA_DISCONNECTED (1u << 2)
#define MYBOT_WIFI_EVENT_GOT_IP (1u << 3)
#define MYBOT_WIFI_EVENT_DHCP_TIMEOUT (1u << 4)
#define MYBOT_WIFI_EVENT_LINK_MASK                                                   \
    (MYBOT_WIFI_EVENT_STA_CONNECTED | MYBOT_WIFI_EVENT_STA_DISCONNECTED |           \
     MYBOT_WIFI_EVENT_GOT_IP | MYBOT_WIFI_EVENT_DHCP_TIMEOUT)

typedef struct {
    bool stopping;
    bool sta_owned;
    bool scan_in_progress;
    bool wifi_event_registered;
    bool netif_event_registered;
    uint32_t event_flags;
    beken_mutex_t lock;
    beken_semaphore_t event;
} mybot_wifi_runtime_t;

int mybot_wifi_runtime_init(mybot_wifi_runtime_t *runtime);
void mybot_wifi_runtime_deinit(mybot_wifi_runtime_t *runtime);

bool mybot_wifi_runtime_lock(mybot_wifi_runtime_t *runtime);
void mybot_wifi_runtime_unlock(mybot_wifi_runtime_t *runtime);
bool mybot_wifi_runtime_is_stopping(mybot_wifi_runtime_t *runtime);
void mybot_wifi_runtime_request_stop(mybot_wifi_runtime_t *runtime);

uint32_t mybot_wifi_time_now(void);
bool mybot_wifi_time_reached(uint32_t now, uint32_t deadline);
uint32_t mybot_wifi_time_remaining(uint32_t now, uint32_t deadline);
bool mybot_wifi_runtime_delay(mybot_wifi_runtime_t *runtime, uint32_t delay_ms);
void mybot_wifi_runtime_wait(mybot_wifi_runtime_t *runtime, uint32_t timeout_ms);

uint32_t mybot_wifi_runtime_take_events(mybot_wifi_runtime_t *runtime, uint32_t mask);
void mybot_wifi_runtime_drain_events(mybot_wifi_runtime_t *runtime, uint32_t mask);

int mybot_wifi_runtime_register_callbacks(mybot_wifi_runtime_t *runtime);
bool mybot_wifi_runtime_unregister_callbacks(mybot_wifi_runtime_t *runtime);

int mybot_wifi_runtime_start_sta(mybot_wifi_runtime_t *runtime,
                                 const mybot_wifi_credential_entry_t *credential,
                                 uint8_t auto_reconnect_count);
bool mybot_wifi_runtime_stop_sta(mybot_wifi_runtime_t *runtime);
void mybot_wifi_runtime_set_sta_owned(mybot_wifi_runtime_t *runtime, bool owned);
bool mybot_wifi_runtime_sta_link_matches(const char *ssid);
bool mybot_wifi_runtime_wait_for_ipv4(mybot_wifi_runtime_t *runtime, const char *ssid,
                                      uint32_t timeout_ms);

int mybot_wifi_runtime_start_scan(mybot_wifi_runtime_t *runtime);
bool mybot_wifi_runtime_stop_scan(mybot_wifi_runtime_t *runtime);
bool mybot_wifi_runtime_is_scan_in_progress(mybot_wifi_runtime_t *runtime);
int mybot_wifi_runtime_collect_scan_results(mybot_wifi_runtime_t *runtime,
                                            wifi_scan_ap_info_t *results, size_t capacity,
                                            size_t *result_count);
int mybot_wifi_runtime_scan_sync(mybot_wifi_runtime_t *runtime,
                                 wifi_scan_ap_info_t *results, size_t capacity,
                                 size_t *result_count, uint32_t timeout_ms);

bool mybot_wifi_string_is_terminated(const void *value, size_t capacity);

#endif /* MYBOT_WIFI_RUNTIME_H_ */
