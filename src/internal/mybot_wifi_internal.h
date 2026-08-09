/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_WIFI_INTERNAL_H_
#define MYBOT_WIFI_INTERNAL_H_

#include <mybot/platform/mybot_wifi.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * SDK-internal Wi-Fi facade. The public mybot/platform/mybot_wifi.h only
 * exposes the backend contract (enums, handler typedefs, ops table and
 * mybot_wifi_register()); the SDK core drives provisioning and state.
 */

/**
 * Start APSTA provisioning without waiting for STA to connect.
 * A successful return means the backend started; subsequent connection results
 * are reported through handler and mybot_wifi_get_state().
 */
int mybot_wifi_init(const char *device_id, mybot_wifi_state_handler_t handler, void *user_data);

/** Return the current provisioning state. */
mybot_wifi_state_t mybot_wifi_get_state(void);

/** Stop APSTA provisioning and release its resources. Idempotent. */
void mybot_wifi_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_WIFI_INTERNAL_H_ */
