/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_WIFI_H_
#define MYBOT_WIFI_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MYBOT_WIFI_EVENT_STA_CONNECTED = 0,
    MYBOT_WIFI_EVENT_STA_DISCONNECTED,
    MYBOT_WIFI_EVENT_FAILED,
} mybot_wifi_event_t;

typedef enum {
    MYBOT_WIFI_STATE_IDLE = 0,
    MYBOT_WIFI_STATE_PROVISIONING,
    MYBOT_WIFI_STATE_CONNECTED,
    MYBOT_WIFI_STATE_DISCONNECTED,
    MYBOT_WIFI_STATE_FAILED,
} mybot_wifi_state_t;

typedef void (*mybot_wifi_event_handler_t)(mybot_wifi_event_t event,
                                                        void *user_data);
typedef void (*mybot_wifi_state_handler_t)(mybot_wifi_state_t state,
                                                        void *user_data);

/**
 * Platform APSTA provisioning operations. The backend owns the provisioning transport and Wi-Fi
 * credential persistence. init() starts AP and STA together and reports connection events through
 * emit. destroy() stops provisioning and waits for in-flight handlers.
 */
typedef struct {
    const char *name;
    int (*init)(void **ctx, const char *device_id, mybot_wifi_event_handler_t emit,
                void *user_data);
    void (*destroy)(void *ctx);
} mybot_wifi_ops_t;

/** Register the APSTA provisioning backend for the current platform. */
int mybot_wifi_register(const mybot_wifi_ops_t *ops);

/**
 * Start APSTA provisioning without waiting for STA to connect.
 * A successful return means the backend started; subsequent connection results are reported
 * through handler and mybot_wifi_get_state().
 */
int mybot_wifi_init(const char *device_id,
                                 mybot_wifi_state_handler_t handler, void *user_data);

/** Return the current provisioning state. */
mybot_wifi_state_t mybot_wifi_get_state(void);

/** Stop APSTA provisioning and release its resources. Idempotent. */
void mybot_wifi_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_WIFI_H_ */
