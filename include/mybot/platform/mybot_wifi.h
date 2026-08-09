/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_WIFI_H_
#define MYBOT_WIFI_H_

#include <mybot/mybot_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Wi-Fi connection events emitted by the platform provisioning backend.
 *
 * Events may be emitted from platform threads. The backend must not emit any
 * event after destroy() returns.
 */
typedef enum {
    /** The STA link connected to the configured access point. */
    MYBOT_WIFI_EVENT_STA_CONNECTED = 0,
    /** The STA link was lost or disconnected at runtime. */
    MYBOT_WIFI_EVENT_STA_DISCONNECTED,
    /** Provisioning or connection failed unrecoverably. */
    MYBOT_WIFI_EVENT_FAILED,
} mybot_wifi_event_t;

/**
 * Lifecycle state of the Wi-Fi provisioning module, as observed by the SDK.
 *
 * The backend emits connection events; the SDK maps them onto these states.
 */
typedef enum {
    /** No backend registered and no provisioning session active. */
    MYBOT_WIFI_STATE_IDLE = 0,
    /** APSTA provisioning is running and waiting for the STA link. */
    MYBOT_WIFI_STATE_PROVISIONING,
    /** The STA link is connected. */
    MYBOT_WIFI_STATE_CONNECTED,
    /** The STA link is down after having been connected. */
    MYBOT_WIFI_STATE_DISCONNECTED,
    /** Provisioning failed and cannot continue. */
    MYBOT_WIFI_STATE_FAILED,
} mybot_wifi_state_t;

/**
 * Backend-to-SDK connection event callback.
 *
 * @param event     the Wi-Fi event that occurred
 * @param user_data opaque pointer passed to the backend at init() time
 *
 * @note Called from platform context; keep it short and do not call
 *       mybot_stop() from inside this callback.
 */
typedef void (*mybot_wifi_event_handler_t)(mybot_wifi_event_t event, void *user_data);

/**
 * SDK-to-application Wi-Fi state change callback.
 *
 * @param state     the new provisioning/link state
 * @param user_data opaque pointer supplied by the application
 *
 * @note Called from SDK internal context; keep it short and do not call
 *       mybot_stop() from inside this callback.
 */
typedef void (*mybot_wifi_state_handler_t)(mybot_wifi_state_t state, void *user_data);

/**
 * Platform APSTA provisioning backend operations.
 *
 * The backend owns the provisioning transport (AP + STA) and Wi-Fi credential
 * persistence. It must keep monitoring the STA link after the first successful
 * connection and report runtime disconnects and reconnects through emit().
 *
 * @note All callbacks may run on platform threads. destroy() must stop the
 *       transport and wait for any in-flight callback to return before it
 *       returns.
 */
typedef struct {
    /** Backend name for logging and diagnostics. */
    const char *name;

    /**
     * Start APSTA provisioning without waiting for the user.
     *
     * @param ctx       [out] backend context handle
     * @param device_id NUL-terminated device identifier forwarded from
     *                  mybot_start()
     * @param emit      callback for reporting connection events
     * @param user_data opaque pointer forwarded to emit(); reserved, pass NULL
     * @return 0 on success, -1 on error
     */
    int (*init)(void **ctx, const char *device_id, mybot_wifi_event_handler_t emit,
                void *user_data);

    /**
     * Stop provisioning and release all resources.
     *
     * Must stop the AP/STA transport and wait for in-flight handlers. No
     * event is emitted after this returns.
     *
     * @param ctx backend context from init()
     */
    void (*destroy)(void *ctx);
} mybot_wifi_ops_t;

/**
 * Register the APSTA provisioning backend for the current platform.
 *
 * @param ops backend operations table; must remain valid for the process
 *            lifetime
 * @return 0 on success, -1 if ops is invalid or a backend is already active
 *
 * @note Call exactly once, before mybot_start().
 */
MYBOT_API int mybot_wifi_register(const mybot_wifi_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_WIFI_H_ */
