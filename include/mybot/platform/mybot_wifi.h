/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_WIFI_H_
#define MYBOT_WIFI_H_

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------
 * Platform Wi-Fi connectivity operations (hook interface)
 *
 * The product owns provisioning, network credentials, and the network stack.
 * These operations attach the SDK to usable connectivity notifications.
 * ---------------------------------------------------------- */

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
    /** Initial connectivity confirmation failed, or runtime connectivity failed. */
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
 * Complete product provisioning and establish usable connectivity before calling
 * mybot_start(). Before entering provisioning again, call mybot_stop() from the
 * product control task and wait for it to return. The SDK neither starts
 * provisioning nor owns the product's network connection.
 *
 * At initialization, report the current usable connection once, then report runtime
 * disconnects and reconnects in transition order. Do not repeat a connected
 * event without an intervening disconnected or failed event. Ordinary link loss
 * and reconnection are handled through these events while the SDK remains running.
 *
 * @note The SDK calls init() and destroy() on its control thread. emit() may run
 *       during init() or on platform threads, but calls must be serialized.
 *       destroy() must prevent further SDK notifications and wait for in-flight
 *       callbacks; it does not shut down the product's network stack.
 */
typedef struct {
    /**
     * Register connectivity monitoring for this SDK run.
     *
     * Report MYBOT_WIFI_EVENT_STA_CONNECTED when the existing connection is usable,
     * including when it was already connected before init(). The first event may
     * be emitted synchronously here or asynchronously after return. Do not start
     * provisioning or wait for user input. If registration fails, release partial
     * resources and stop any callbacks before returning an error.
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
     * Remove this SDK run's connectivity listener and release its resources.
     *
     * Prevent new emit() calls and wait for any in-flight handler to return.
     * No SDK event is emitted after this returns. Keep the product's connection,
     * network stack, and saved credentials under product control.
     *
     * @param ctx implementation context from init()
     */
    void (*destroy)(void *ctx);
} mybot_wifi_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_WIFI_H_ */
