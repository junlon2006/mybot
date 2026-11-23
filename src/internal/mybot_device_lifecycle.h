/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_DEVICE_LIFECYCLE_H_
#define MYBOT_DEVICE_LIFECYCLE_H_

#include "mybot_device_client.h"
#include "mybot_device_state.h"
#include "mybot_kv_store_internal.h"

#include <api/aosl_atomic.h>

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MYBOT_DEVICE_SERVICE_PAIR_POLL,
    MYBOT_DEVICE_SERVICE_DEVICE_POLL,
    MYBOT_DEVICE_SERVICE_STOP,
} mybot_device_service_request_type_t;

/** Heap-owned HTTP work; only the control owner applies its result. */
typedef struct {
    mybot_device_service_request_type_t type;
    intptr_t network_generation;
    uint32_t binding_generation;
    char server_base[MYBOT_DEVICE_CLIENT_MAX_URL];
    char device_id[MYBOT_DEVICE_CLIENT_MAX_ID];
    char credential[MYBOT_DEVICE_CLIENT_MAX_TOKEN];
    int result;

    union {
        mybot_device_binding_t binding;

        struct {
            char conversation_id[MYBOT_DEVICE_CLIENT_MAX_ID];
            char reason[32];
        } stop;
    } payload;
} mybot_device_service_request_t;

/* Callbacks invoked by the state machine onto the app layer */
typedef struct {
    /** A pair code was obtained; present it through the device UI and/or speaker. */
    void (*on_pair_code)(const char *code, void *user_data);

    /**
     * Start a conversation using the device-service response.
     *
     * Called synchronously. The response is borrowed until this callback returns;
     * the lifecycle then frees it. Copy any fields needed after the callback.
     */
    void (*on_conversation_start)(const mybot_device_conversation_t *params, void *user_data);

    /** Stop local media and leave RTC synchronously, before the service stop request. */
    void (*on_conversation_stop)(void *user_data);

    /** Apply a renewed RTC token. Return 0 when the RTC SDK accepts it. */
    int (*on_rtc_token_renewed)(const char *token, void *user_data);

    /** State changed (for logging / UI). */
    void (*on_state_changed)(mybot_device_state_t state, void *user_data);

    /** Queue HTTP work. Success transfers ownership; failure leaves it with the caller. */
    int (*submit_service_request)(mybot_device_service_request_t *request, void *user_data);

    /** Opaque owner context passed to every callback. */
    void *user_data;
} mybot_device_lifecycle_callbacks_t;

/** Caller-owned state for one device lifecycle instance. */
typedef struct {
    char server_base[MYBOT_DEVICE_CLIENT_MAX_URL];
    char device_id[MYBOT_DEVICE_CLIENT_MAX_ID];
    char firmware_ver[64];
    char hw_model[64];
    mybot_device_lifecycle_callbacks_t cbs;
    mybot_kv_store_t *kv_store;

    aosl_atomic_t state;

    char pair_token[MYBOT_DEVICE_CLIENT_MAX_TOKEN];
    int pair_poll_interval;
    int pair_tick_counter;
    int pair_retry_delay_ticks;
    int pair_retry_ticks_remaining;

    char device_token[MYBOT_DEVICE_CLIENT_MAX_TOKEN];
    int runtime_poll_interval;
    int runtime_tick_counter;
    bool poll_pending;
    uint32_t binding_generation;

    char conversation_id[MYBOT_DEVICE_CLIENT_MAX_ID];
    char rtc_channel[128];
    char rtc_uid[64];
    bool conversation_requested;
    aosl_atomic_t stop_request;
    aosl_atomic_t rtc_token_renewal_requested;
    bool rtc_token_renewal_pending;
    int rtc_token_retry_delay_ticks;
    int rtc_token_retry_ticks_remaining;

    bool pairing_requested;
    aosl_atomic_t shutting_down;
    aosl_atomic_t network_available;
    aosl_atomic_t network_loss_pending;
    aosl_atomic_t network_generation;
} mybot_device_lifecycle_t;

/** Initialize a caller-owned lifecycle. Returns 0 on success, -1 on error. */
int mybot_device_lifecycle_init(mybot_device_lifecycle_t *lifecycle, mybot_kv_store_t *kv_store,
                                const char *server_base, const char *device_id,
                                const char *firmware_ver, const char *hw_model,
                                const mybot_device_lifecycle_callbacks_t *cbs);

/** Called every 100 ms by the application control owner.
 *  Drives polling and state transitions; calls must be serialized. */
void mybot_device_lifecycle_tick(mybot_device_lifecycle_t *lifecycle);

/** Apply borrowed background HTTP results on the application control owner.
 *  The caller frees the request after this function returns. */
void mybot_device_lifecycle_service_result(mybot_device_lifecycle_t *lifecycle,
                                           const mybot_device_service_request_t *request);

/** Publish an atomic network snapshot from any thread; tick() consumes it on control. */
void mybot_device_lifecycle_set_network_available(mybot_device_lifecycle_t *lifecycle,
                                                  bool available);

/** Stop state-machine activity and close any active conversation.
 *  Must be called by the application control owner. */
void mybot_device_lifecycle_shutdown(mybot_device_lifecycle_t *lifecycle);

/* Pair, start, and stop requests must be serialized by the application control owner. */

/** Request a fresh pairing flow. */
void mybot_device_lifecycle_request_pair(mybot_device_lifecycle_t *lifecycle);

/** Trigger conversation start. */
void mybot_device_lifecycle_request_start(mybot_device_lifecycle_t *lifecycle);

/** Trigger conversation stop. */
void mybot_device_lifecycle_request_stop(mybot_device_lifecycle_t *lifecycle);

/** Publish from an RTC callback that the active connection ended. */
void mybot_device_lifecycle_notify_conversation_ended(mybot_device_lifecycle_t *lifecycle);

/** Request RTC-token renewal for the active conversation. May be called from any thread. */
void mybot_device_lifecycle_request_rtc_token_renewal(mybot_device_lifecycle_t *lifecycle);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_DEVICE_LIFECYCLE_H_ */
