/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_PROVISIONING_H_
#define MYBOT_PROVISIONING_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MYBOT_PROVISIONING_STATE_IDLE = 0,
    MYBOT_PROVISIONING_STATE_RUNNING,
    MYBOT_PROVISIONING_STATE_COMPLETED,
    MYBOT_PROVISIONING_STATE_FAILED,
} mybot_provisioning_state_t;

/* Starts the APSTA portal. generation must uniquely identify this instance. */
int mybot_provisioning_start(const char *device_id, uint32_t generation);

/* Stops SoftAP and temporary STA, unregisters callbacks, and joins the worker. */
int mybot_provisioning_stop(void);

/* Authoritative state used to recover if a notification cannot be queued. */
mybot_provisioning_state_t mybot_provisioning_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_PROVISIONING_H_ */
