/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_WIFI_INTERNAL_H_
#define MYBOT_WIFI_INTERNAL_H_

#include <mybot/platform/mybot_wifi.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * SDK-internal Wi-Fi facade. The public mybot/platform/mybot_wifi.h only
 * exposes the platform contract (event and handler types, ops table and
 * mybot_wifi_register()); the SDK core drives provisioning and state.
 */

/**
 * Start the platform Wi-Fi workflow without waiting for the STA link.
 * A successful return means the implementation started; subsequent connectivity
 * results are reported through handler.
 */
int mybot_wifi_init(const char *device_id, mybot_wifi_event_handler_t handler, void *user_data);

/** Stop provisioning/link monitoring and release its resources. Idempotent. */
void mybot_wifi_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_WIFI_INTERNAL_H_ */
