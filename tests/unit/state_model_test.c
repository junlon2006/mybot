/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_state_model.h"

#include <assert.h>

int main(void) {
    mybot_state_model_t model;
    mybot_state_model_reset(&model);
    assert(mybot_state_model_get(&model) == MYBOT_STATE_STOPPED);
    assert(mybot_state_model_get_view(&model).device_state == MYBOT_DEVICE_STATE_UNPROVISIONED);

    assert(mybot_state_model_begin_start(&model));
    assert(mybot_state_model_get(&model) == MYBOT_STATE_WIFI_PROVISIONING);
    assert(!mybot_state_model_services_ready(&model));

    assert(mybot_state_model_begin_services(&model));
    assert(mybot_state_model_get(&model) == MYBOT_STATE_STARTING_SERVICES);
    assert(mybot_state_model_set_device_state(&model, MYBOT_DEVICE_STATE_RUNTIME));
    assert(mybot_state_model_services_ready(&model));
    assert(mybot_state_model_get(&model) == MYBOT_STATE_READY);

    assert(mybot_state_model_set_device_state(&model, MYBOT_DEVICE_STATE_IN_CONVERSATION));
    assert(mybot_state_model_get(&model) == MYBOT_STATE_IN_CONVERSATION);

    assert(mybot_state_model_network_lost(&model));
    assert(mybot_state_model_get(&model) == MYBOT_STATE_WIFI_DISCONNECTED);
    assert(mybot_state_model_get_view(&model).device_state == MYBOT_DEVICE_STATE_RUNTIME);

    /* Offline device callbacks cannot override the connectivity projection. */
    assert(mybot_state_model_set_device_state(&model, MYBOT_DEVICE_STATE_IN_CONVERSATION));
    assert(mybot_state_model_get(&model) == MYBOT_STATE_WIFI_DISCONNECTED);
    assert(mybot_state_model_network_restored(&model));
    assert(mybot_state_model_get(&model) == MYBOT_STATE_READY);
    assert(mybot_state_model_get_view(&model).device_state == MYBOT_DEVICE_STATE_RUNTIME);

    assert(mybot_state_model_set_device_state(&model, MYBOT_DEVICE_STATE_AWAITING_CLAIM));
    assert(mybot_state_model_get(&model) == MYBOT_STATE_READY);

    assert(mybot_state_model_fail(&model));
    assert(mybot_state_model_get(&model) == MYBOT_STATE_FAILED);
    assert(!mybot_state_model_fail(&model));

    mybot_state_model_begin_stop(&model);
    assert(mybot_state_model_get(&model) == MYBOT_STATE_STOPPING);
    assert(!mybot_state_model_fail(&model));

    mybot_state_model_stopped(&model);
    assert(mybot_state_model_get(&model) == MYBOT_STATE_STOPPED);
    assert(!mybot_state_model_network_lost(&model));
    return 0;
}
