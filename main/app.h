#ifndef MYBOT_APP_H_
#define MYBOT_APP_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Application configuration (parsed from command line) */
typedef struct {
    char  server_base[128];   /* device API server URL */
    char  device_id[64];      /* unique device identifier */
    char  firmware_ver[32];   /* firmware version (optional) */
    char  hw_model[32];       /* hardware model (optional) */
} app_config_t;

/** Start the mybot application.
 *  Blocks until stopped.
 */
int app_start(const app_config_t *cfg);

/** Signal the application to stop gracefully. */
void app_stop(void);

/** Check if the application is running. */
bool app_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_APP_H_ */
