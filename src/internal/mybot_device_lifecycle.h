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

/* Conversation parameters (from server response) */
typedef struct {
    char conversation_id[MYBOT_DEVICE_CLIENT_MAX_ID];
    char rtc_app_id[64];
    char rtc_channel[128];
    char rtc_uid[64];       /* string UID assigned by server */
    char rtc_agent_uid[64]; /* string RTM peer UID assigned by server */
    char rtc_token[MYBOT_DEVICE_CLIENT_MAX_TOKEN];
} mybot_conversation_params_t;

/* Callbacks invoked by the state machine onto the app layer */
typedef struct {
    /** A pair code was obtained; present it through the device UI and/or speaker. */
    void (*on_pair_code)(const char *code, void *user_data);

    /** Conversation should start — join RTC channel with given params. */
    void (*on_conversation_start)(const mybot_conversation_params_t *params, void *user_data);

    /** Conversation should stop — leave RTC channel. */
    void (*on_conversation_stop)(void *user_data);

    /** Apply a renewed RTC token. Return 0 when the RTC SDK accepts it. */
    int (*on_rtc_token_renewed)(const char *token, void *user_data);

    /** State changed (for logging / UI). */
    void (*on_state_changed)(mybot_device_state_t state, void *user_data);

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

    char conversation_id[MYBOT_DEVICE_CLIENT_MAX_ID];
    char rtc_channel[128];
    char rtc_uid[64];
    char rtc_agent_uid[64];
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

/* ----------------------------------------------------------
 * API
 * ---------------------------------------------------------- */

/** Initialize the device state machine.
 *  @param lifecycle    caller-owned lifecycle context
 *  @param kv_store     initialized credential store for this runtime
 *  @param server_base  Device-service base URL.
 *  @param device_id    Unique device identifier (e.g. "AG-A1B2C3")
 *  @param firmware_ver Firmware version string (may be NULL)
 *  @param hw_model     Hardware model string (may be NULL)
 *  @param cbs          Callbacks (may be NULL)
 *  @return 0 on success, -1 on error.
 */
int mybot_device_lifecycle_init(mybot_device_lifecycle_t *lifecycle, mybot_kv_store_t *kv_store,
                                const char *server_base, const char *device_id,
                                const char *firmware_ver, const char *hw_model,
                                const mybot_device_lifecycle_callbacks_t *cbs);

/** Called every 100 ms by the application control owner.
 *  Drives polling and state transitions; calls must be serialized. */
void mybot_device_lifecycle_tick(mybot_device_lifecycle_t *lifecycle);

/**
 * Notify the state machine whether device-service networking is available.
 * May be called from any thread as an atomic network input. While offline, tick()
 * performs no HTTP actions; the control owner ends an active conversation locally.
 */
void mybot_device_lifecycle_set_network_available(mybot_device_lifecycle_t *lifecycle,
                                                  bool available);

/** Stop state-machine activity and close any active conversation.
 *  Must be called by the application control owner. */
void mybot_device_lifecycle_shutdown(mybot_device_lifecycle_t *lifecycle);

/** Request a fresh pairing flow from the application control owner. */
void mybot_device_lifecycle_request_pair(mybot_device_lifecycle_t *lifecycle);

/** Trigger conversation start from the application control owner. */
void mybot_device_lifecycle_request_start(mybot_device_lifecycle_t *lifecycle);

/** Trigger conversation stop from the application control owner. */
void mybot_device_lifecycle_request_stop(mybot_device_lifecycle_t *lifecycle);

/** Publish from an RTC callback that the active connection ended. */
void mybot_device_lifecycle_notify_conversation_ended(mybot_device_lifecycle_t *lifecycle);

/** Request RTC-token renewal for the active conversation. May be called from any thread. */
void mybot_device_lifecycle_request_rtc_token_renewal(mybot_device_lifecycle_t *lifecycle);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_DEVICE_LIFECYCLE_H_ */
