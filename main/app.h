#ifndef MYBOT_APP_H_
#define MYBOT_APP_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Application configuration (parsed from the command line). */
typedef struct {
    char  server_base[128];   /* device API server base URL */
    char  device_id[64];      /* unique device identifier */
    char  firmware_ver[32];   /* firmware version (optional) */
    char  hw_model[32];       /* hardware model (optional) */
} app_config_t;

/**
 * @brief Initialize and start the application.
 *
 * Non-blocking: this spawns the worker threads (audio capture/playback and
 * the MPQ loop) and returns. The application then runs on its own, driven by
 * its MPQ timers; the caller must call app_stop() before exiting to release
 * all resources.
 *
 * @param cfg configuration; must remain valid for the application lifetime.
 * @return 0 on success, -1 on error. app_stop() may still be called to
 *         release any resources allocated before the failure.
 */
int app_start(const app_config_t *cfg);

/** @brief Check whether the application is still running. */
bool app_is_running(void);

/** @brief Request a graceful exit (used by signal handlers / UI keys). */
void app_request_exit(void);

/** @brief User requests to start a conversation (button / key). */
void app_start_conversation(void);

/** @brief User requests to stop the current conversation. */
void app_stop_conversation(void);

/** @brief User requests to (re-)pair the device. */
void app_pair(void);

/**
 * @brief Stop the application and release all resources.
 *
 * Blocks until all worker threads have exited. Idempotent, and safe to call
 * even if app_start() failed partway through initialization.
 */
void app_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_APP_H_ */
