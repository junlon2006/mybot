#ifndef MYBOT_WIFI_PROVISIONING_H_
#define MYBOT_WIFI_PROVISIONING_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MYBOT_WIFI_PROVISIONING_EVENT_STA_CONNECTED = 0,
    MYBOT_WIFI_PROVISIONING_EVENT_STA_DISCONNECTED,
    MYBOT_WIFI_PROVISIONING_EVENT_FAILED,
} mybot_wifi_provisioning_event_t;

typedef void (*mybot_wifi_provisioning_event_handler_t)(mybot_wifi_provisioning_event_t event,
                                                        void *user_data);

/**
 * Platform APSTA provisioning operations. The backend owns the provisioning transport and Wi-Fi
 * credential persistence. init() starts AP and STA together and reports connection events through
 * emit. destroy() stops provisioning and waits for in-flight handlers.
 */
typedef struct {
    const char *name;
    int (*init)(void **ctx, const char *device_id, mybot_wifi_provisioning_event_handler_t emit,
                void *user_data);
    void (*destroy)(void *ctx);
} mybot_wifi_provisioning_ops_t;

/** Register the APSTA provisioning backend for the current platform. */
int mybot_wifi_provisioning_register(const mybot_wifi_provisioning_ops_t *ops);

/** Start APSTA provisioning and block until STA connects or provisioning fails. */
int mybot_wifi_provisioning_init(const char *device_id);

/** Return whether the backend currently reports an active STA connection. */
bool mybot_wifi_provisioning_is_connected(void);

/** Stop APSTA provisioning and release its resources. Idempotent. */
void mybot_wifi_provisioning_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_WIFI_PROVISIONING_H_ */
