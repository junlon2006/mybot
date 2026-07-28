#ifndef MYBOT_APP_H_
#define MYBOT_APP_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Application configuration (parsed from command line) */
typedef struct {
    char  app_id[64];
    char  channel[64];
    char  token[256];
    char  user[64];           /* string user account for RTC */
} app_config_t;

/** Start the mybot application.
 *  Initializes AOSL, audio subsystem, and RTC session.
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
