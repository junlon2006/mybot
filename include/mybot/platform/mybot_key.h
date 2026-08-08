/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_KEY_SERVICE_H_
#define MYBOT_KEY_SERVICE_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MYBOT_KEY_EVENT_CONVERSATION_START = 0,
    MYBOT_KEY_EVENT_CONVERSATION_STOP,
    MYBOT_KEY_EVENT_PAIR,
    MYBOT_KEY_EVENT_EXIT,
    /* Optional: adjust the SDK-managed media volume. */
    MYBOT_KEY_EVENT_VOLUME_UP,
    MYBOT_KEY_EVENT_VOLUME_DOWN,
} mybot_key_event_t;

typedef void (*mybot_key_event_handler_t)(mybot_key_event_t event, void *user_data);

/**
 * Platform key input operations. init() starts the backend event source; destroy() stops it
 * and waits for any in-flight event handler to return.
 */
typedef struct {
    const char *name;
    int (*init)(void **ctx, mybot_key_event_handler_t emit, void *user_data);
    void (*destroy)(void *ctx);
} mybot_key_service_ops_t;

/** Register the key backend for the current platform. Call before service initialization. */
int mybot_key_service_register(const mybot_key_service_ops_t *ops);

/** Initialize the registered backend and install the application event handler. */
int mybot_key_service_init(mybot_key_event_handler_t handler, void *user_data);

/** Stop the backend and release its resources. No events are emitted after return. */
void mybot_key_service_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_KEY_SERVICE_H_ */
