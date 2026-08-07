/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_DEVICE_LIFECYCLE_H_
#define MYBOT_DEVICE_LIFECYCLE_H_

#include "mybot_device_client.h"

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------
 * Device lifecycle states
 * ---------------------------------------------------------- */
typedef enum {
    MYBOT_DEVICE_STATE_UNPROVISIONED,   /* no device_token, need pairing */
    MYBOT_DEVICE_STATE_PAIRING,         /* requesting a pair code */
    MYBOT_DEVICE_STATE_AWAITING_CLAIM,  /* waiting for the device to be claimed */
    MYBOT_DEVICE_STATE_RUNTIME,         /* have device_token, idle */
    MYBOT_DEVICE_STATE_IN_CONVERSATION, /* active RTC call */
} mybot_device_state_t;

/* Conversation parameters (from server response) */
typedef struct {
    char conversation_id[MYBOT_DEVICE_CLIENT_MAX_ID];
    char rtc_app_id[64];
    char rtc_channel[128];
    char rtc_uid[64]; /* string UID assigned by server */
    char rtc_token[MYBOT_DEVICE_CLIENT_MAX_TOKEN];
} mybot_conversation_params_t;

/* Callbacks invoked by the state machine onto the app layer */
typedef struct {
    /** A pair code was obtained — device should TTS-broadcast it. */
    void (*on_pair_code)(const char *code);

    /** Conversation should start — join RTC channel with given params. */
    void (*on_conversation_start)(const mybot_conversation_params_t *params);

    /** Conversation should stop — leave RTC channel. */
    void (*on_conversation_stop)(void);

    /** State changed (for logging / UI). */
    void (*on_state_changed)(mybot_device_state_t state);
} mybot_device_lifecycle_callbacks_t;

/* ----------------------------------------------------------
 * API
 * ---------------------------------------------------------- */

/** Initialize the device state machine.
 *  @param server_base  HTTPS device-service URL.
 *  @param device_id    Unique device identifier (e.g. "AG-A1B2C3")
 *  @param firmware_ver Firmware version string (may be NULL)
 *  @param hw_model     Hardware model string (may be NULL)
 *  @param cbs          Callbacks (may be NULL)
 *  @return 0 on success, -1 on error.
 */
int mybot_device_lifecycle_init(const char *server_base, const char *device_id,
                                const char *firmware_ver, const char *hw_model,
                                mybot_device_lifecycle_callbacks_t *cbs);

/** Must be called periodically from the main loop (e.g., every 100ms).
 *  Drives polling and state transitions. */
void mybot_device_lifecycle_tick(void);

/**
 * Notify the state machine whether device-service networking is available.
 * May be called from any thread. While offline, tick() performs no HTTP actions;
 * an active conversation is ended locally on the state-machine thread.
 */
void mybot_device_lifecycle_set_network_available(bool available);

/** Stop state-machine activity and close any active conversation.
 *  Must be called from the same thread that calls mybot_device_lifecycle_tick(). */
void mybot_device_lifecycle_shutdown(void);

/** Get current state. */
mybot_device_state_t mybot_device_lifecycle_get_state(void);

/** Return human-readable state name. */
const char *mybot_device_lifecycle_state_name(mybot_device_state_t s);

/** Return the current device_token (NULL if not in runtime). */
const char *mybot_device_lifecycle_get_token(void);

/** Trigger pairing from unprovisioned state. */
void mybot_device_lifecycle_request_pair(void);

/** Trigger conversation start (user pressed button). */
void mybot_device_lifecycle_request_start(void);

/** Trigger conversation stop (user hung up). */
void mybot_device_lifecycle_request_stop(void);

/** Notify state machine that conversation RTC connection ended. */
void mybot_device_lifecycle_notify_conversation_ended(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_DEVICE_LIFECYCLE_H_ */
