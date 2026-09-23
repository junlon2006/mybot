/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_WIFI_INTERNAL_H_
#define MYBOT_WIFI_INTERNAL_H_

#include <mybot/platform/mybot_wifi.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const mybot_wifi_ops_t *ops;
    void *ctx;
    bool active;
} mybot_wifi_t;

/**
 * SDK-internal Wi-Fi facade. The public mybot/platform/mybot_wifi.h only
 * exposes the platform contract (event and handler types, ops table and
 * mybot_platform_register()); the SDK consumes connectivity events, while the
 * product owns provisioning and network credentials.
 */

/**
 * Register the platform connectivity monitor. Initial usable connectivity and
 * subsequent transitions are reported through handler.
 */
int mybot_wifi_init(mybot_wifi_t *wifi, const char *device_id, mybot_wifi_event_handler_t handler,
                    void *user_data);

/** Unregister connectivity notifications and wait for callbacks. Idempotent. */
void mybot_wifi_deinit(mybot_wifi_t *wifi);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_WIFI_INTERNAL_H_ */
