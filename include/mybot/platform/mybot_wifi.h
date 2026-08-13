/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_WIFI_H_
#define MYBOT_WIFI_H_

#include <mybot/mybot_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Network connectivity events emitted by the platform Wi-Fi implementation.
 *
 * Events may be emitted from platform threads, but must be delivered serially and
 * in transition order. The implementation must not emit any event after destroy()
 * returns.
 */
typedef enum {
    /** The STA has an IP address and usable network connectivity for SDK traffic. */
    MYBOT_WIFI_EVENT_STA_CONNECTED = 0,
    /** Usable STA network connectivity was lost at runtime. */
    MYBOT_WIFI_EVENT_STA_DISCONNECTED,
    /** Provisioning failed unrecoverably, or usable connectivity failed at runtime. */
    MYBOT_WIFI_EVENT_FAILED,
} mybot_wifi_event_t;

/**
 * Platform-to-SDK connection event callback.
 *
 * @param event     the Wi-Fi event that occurred
 * @param user_data opaque pointer passed to the implementation at init() time
 *
 * @note Called from platform context; keep it short and do not call
 *       mybot_stop() from inside this callback.
 */
typedef void (*mybot_wifi_event_handler_t)(mybot_wifi_event_t event, void *user_data);

/**
 * Platform Wi-Fi connectivity operations.
 *
 * APSTA provisioning is the project's recommended production model and current
 * preferred solution. Platform ports should use APSTA wherever available so
 * onboarding, connection transitions, and recovery behavior remain consistent
 * across products. Alternative platform implementations remain supported for development hosts
 * or platforms that cannot provide APSTA.
 *
 * The implementation owns the platform-specific connection or provisioning
 * workflow and, where applicable, Wi-Fi credential persistence. It must keep
 * monitoring usable network connectivity after the first successful connection
 * and report runtime disconnects and reconnects through emit(). It must emit
 * only connectivity transitions; a connected event must not be repeated until
 * after a disconnected or failed event.
 *
 * @note All callbacks may run on platform threads. destroy() must stop the
 *       transport and wait for any in-flight callback to return before it
 *       returns.
 */
typedef struct {
    /** Implementation name for logging and diagnostics. */
    const char *name;

    /**
     * Start the platform Wi-Fi workflow without waiting for network connectivity.
     * Production implementations should normally start APSTA provisioning.
     *
     * @param ctx       [out] implementation context handle
     * @param device_id NUL-terminated device identifier forwarded from
     *                  mybot_start()
     * @param emit      callback for reporting connectivity transition events
     * @param user_data opaque pointer that must be forwarded unchanged to emit()
     * @return 0 on success, -1 on error
     */
    int (*init)(void **ctx, const char *device_id, mybot_wifi_event_handler_t emit,
                void *user_data);

    /**
     * Stop provisioning and release all resources.
     *
     * Must stop provisioning and link monitoring, then wait for in-flight
     * handlers. No event is emitted after this returns.
     *
     * @param ctx implementation context from init()
     */
    void (*destroy)(void *ctx);
} mybot_wifi_ops_t;

/**
 * Register the Wi-Fi connectivity implementation for the current platform.
 *
 * @param ops implementation operations table; must remain valid for the process
 *            lifetime
 * @return 0 on success, -1 if ops is invalid or an implementation is already active
 *
 * @note Call exactly once, before mybot_start().
 */
MYBOT_API int mybot_wifi_register(const mybot_wifi_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_WIFI_H_ */
