/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_KEY_H_
#define MYBOT_KEY_H_

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------
 * Platform key input operations (hook interface)
 *
 * The platform translates hardware input into semantic key events and emits
 * them to the SDK through the registered callback.
 * ---------------------------------------------------------- */

/**
 * Semantic key events translated from hardware input by the platform implementation.
 */
typedef enum {
    /** Start a conversation. */
    MYBOT_KEY_EVENT_CONVERSATION_START = 0,
    /** Stop the active conversation. */
    MYBOT_KEY_EVENT_CONVERSATION_STOP,
    /** Re-pair the device with the service. */
    MYBOT_KEY_EVENT_PAIR,
    /** Request a graceful application exit. */
    MYBOT_KEY_EVENT_EXIT,
    /** Raise the active playback volume. Optional. */
    MYBOT_KEY_EVENT_VOLUME_UP,
    /** Lower the active playback volume. Optional. */
    MYBOT_KEY_EVENT_VOLUME_DOWN,
} mybot_key_event_t;

/**
 * Platform-to-SDK key event callback.
 *
 * @param event     the key event that occurred
 * @param user_data opaque pointer supplied to the implementation at init() time
 *
 * @note Called from platform context; keep it short and do not call
 *       mybot_stop() from inside this callback.
 */
typedef void (*mybot_key_event_handler_t)(mybot_key_event_t event, void *user_data);

/**
 * Platform key input operations.
 *
 * init() starts the implementation event source; destroy() stops it and waits for
 * any in-flight event handler to return.
 */
typedef struct {
    /**
     * Allocate and start the key event source.
     *
     * @param ctx       [out] implementation context handle
     * @param emit      callback for reporting key events
     * @param user_data opaque context forwarded unchanged to emit()
     * @return 0 on success, -1 on error
     *
     * @note user_data remains valid until destroy() returns.
     */
    int (*init)(void **ctx, mybot_key_event_handler_t emit, void *user_data);

    /**
     * Stop the key event source and release all resources.
     *
     * @param ctx implementation context from init()
     *
     * @note The implementation must stop the input source and wait for in-flight handlers. It
     *       must not emit an event after this callback returns.
     */
    void (*destroy)(void *ctx);
} mybot_key_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_KEY_H_ */
