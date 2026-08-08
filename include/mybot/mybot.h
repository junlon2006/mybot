/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_H_
#define MYBOT_H_

#include <stdint.h>
#include <stdbool.h>
#include <mybot/mybot_errors.h>

#ifdef __cplusplus
extern "C" {
#endif

/** SDK configuration supplied by the host application. */
typedef struct {
    char server_base[128]; /* HTTPS service base URL */
    char device_id[64];    /* unique device identifier */
    char firmware_ver[32]; /* firmware version (optional) */
    char hw_model[32];     /* hardware model (optional) */
} mybot_app_config_t;

typedef enum {
    MYBOT_APP_STATE_STOPPED = 0,
    MYBOT_APP_STATE_WIFI_PROVISIONING,
    MYBOT_APP_STATE_STARTING_SERVICES,
    MYBOT_APP_STATE_READY,
    MYBOT_APP_STATE_WIFI_DISCONNECTED,
    MYBOT_APP_STATE_FAILED,
    MYBOT_APP_STATE_STOPPING,
} mybot_app_state_t;

/**
 * @brief Initialize and start the application.
 *
 * Non-blocking: starts APSTA Wi-Fi provisioning and returns. The remaining services are
 * initialized asynchronously after Wi-Fi reaches MYBOT_WIFI_STATE_CONNECTED.
 * The caller must call mybot_app_stop() before exiting.
 *
 * @param cfg application configuration.
 * @return 0 on success, -1 on error. On error, all partially initialized
 *         resources have already been released.
 */
int mybot_app_start(const mybot_app_config_t *cfg);

/** @brief Check whether the application is still running. */
bool mybot_app_is_running(void);

/** @brief Return the current application startup/runtime state. */
mybot_app_state_t mybot_app_get_state(void);

/** @brief Request a graceful exit (used by signal handlers / UI keys). */
void mybot_app_request_exit(void);

/** @brief User requests to start a conversation (button / key). */
void mybot_app_start_conversation(void);

/** @brief User requests to stop the current conversation. */
void mybot_app_stop_conversation(void);

/** @brief User requests to (re-)pair the device. */
void mybot_app_pair(void);

/**
 * @brief Stop the application and release all resources.
 *
 * Blocks until all worker threads have exited. Idempotent.
 */
void mybot_app_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_H_ */
