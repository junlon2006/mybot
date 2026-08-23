/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_KEY_H_
#define MYBOT_KEY_H_

#include <mybot/mybot_export.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    /** Implementation name for logging and diagnostics. */
    const char *name;

    /**
     * Allocate and start the key event source.
     *
     * @param ctx       [out] implementation context handle
     * @param emit      callback for reporting key events
     * @param user_data opaque pointer forwarded to emit(); reserved, pass NULL
     * @return 0 on success, -1 on error
     */
    int (*init)(void **ctx, mybot_key_event_handler_t emit, void *user_data);

    /**
     * Stop the key event source and release all resources.
     *
     * Must stop the input source and wait for in-flight handlers. No event is
     * emitted after this returns.
     *
     * @param ctx implementation context from init()
     */
    void (*destroy)(void *ctx);
} mybot_key_ops_t;

/**
 * Register key-input ops through the legacy per-capability API.
 *
 * @param ops key operations table; must remain valid for the process
 *            lifetime
 * @return 0 on success; -1 if ops is invalid, key input is already registered,
 *         registration is locked, or descriptor registration is active
 *
 * @note Compatibility entry point for existing ports. New ports should include
 *       mybot_platform.h and use mybot_platform_register(). Call before mybot_start().
 *       The first successful per-capability registration selects legacy mode and
 *       prevents later descriptor registration; a successful descriptor registration
 *       likewise prevents this call.
 */
MYBOT_API int mybot_key_register(const mybot_key_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_KEY_H_ */
