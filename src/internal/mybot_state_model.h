/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_STATE_MODEL_H_
#define MYBOT_STATE_MODEL_H_

#include "mybot_device_state.h"

#include <mybot/mybot.h>

#include <api/aosl_atomic.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Atomic application-state reducer input and projection. */
typedef struct {
    aosl_atomic_t snapshot;
} mybot_state_model_t;

typedef struct {
    mybot_state_t app_state;
    mybot_device_state_t device_state;
} mybot_state_view_t;

void mybot_state_model_reset(mybot_state_model_t *model);
bool mybot_state_model_begin_start(mybot_state_model_t *model);
bool mybot_state_model_begin_services(mybot_state_model_t *model);
bool mybot_state_model_services_ready(mybot_state_model_t *model);
bool mybot_state_model_network_lost(mybot_state_model_t *model);
bool mybot_state_model_network_restored(mybot_state_model_t *model);
bool mybot_state_model_set_device_state(mybot_state_model_t *model,
                                        mybot_device_state_t device_state);
bool mybot_state_model_fail(mybot_state_model_t *model);
void mybot_state_model_begin_stop(mybot_state_model_t *model);

mybot_state_view_t mybot_state_model_get_view(const mybot_state_model_t *model);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_STATE_MODEL_H_ */
