#ifndef MYBOT_APP_H_
#define MYBOT_APP_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Application configuration (parsed from the command line). */
typedef struct {
    char server_base[128]; /* service base URL */
    char device_id[64];    /* unique device identifier */
    char firmware_ver[32]; /* firmware version (optional) */
    char hw_model[32];     /* hardware model (optional) */
} mybot_app_config_t;

/**
 * @brief Initialize and start the application.
 *
 * Non-blocking: this spawns the worker threads (audio capture/playback and
 * the MPQ loop) and returns. The application then runs on its own, driven by
 * its MPQ timers; the caller must call mybot_app_stop() before exiting to release
 * all resources.
 *
 * @param cfg application configuration.
 * @return 0 on success, -1 on error. On error, all partially initialized
 *         resources have already been released.
 */
int mybot_app_start(const mybot_app_config_t *cfg);

/** @brief Check whether the application is still running. */
bool mybot_app_is_running(void);

/** @brief Process pending platform input events. */
void mybot_app_poll(void);

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

#endif /* MYBOT_APP_H_ */
