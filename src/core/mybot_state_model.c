/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_state_model.h"

#include <stdint.h>

typedef enum {
    MYBOT_PHASE_STOPPED = 0,
    MYBOT_PHASE_WIFI_PROVISIONING,
    MYBOT_PHASE_STARTING_SERVICES,
    MYBOT_PHASE_RUNNING,
    MYBOT_PHASE_FAILED,
    MYBOT_PHASE_STOPPING,
} mybot_runtime_phase_t;

#define PHASE_SHIFT 0U
#define PHASE_MASK ((uintptr_t)0x7U << PHASE_SHIFT)
#define ONLINE_SHIFT 3U
#define ONLINE_MASK ((uintptr_t)0x1U << ONLINE_SHIFT)
#define DEVICE_SHIFT 4U
#define DEVICE_MASK ((uintptr_t)0x7U << DEVICE_SHIFT)

static uintptr_t snapshot_phase(uintptr_t snapshot) {
    return (snapshot & PHASE_MASK) >> PHASE_SHIFT;
}

static bool snapshot_online(uintptr_t snapshot) {
    return (snapshot & ONLINE_MASK) != 0;
}

static mybot_device_state_t snapshot_device_state(uintptr_t snapshot) {
    return (mybot_device_state_t)((snapshot & DEVICE_MASK) >> DEVICE_SHIFT);
}

static uintptr_t with_phase(uintptr_t snapshot, mybot_runtime_phase_t phase) {
    return (snapshot & ~PHASE_MASK) | ((uintptr_t)phase << PHASE_SHIFT);
}

static uintptr_t with_online(uintptr_t snapshot, bool online) {
    return online ? snapshot | ONLINE_MASK : snapshot & ~ONLINE_MASK;
}

static uintptr_t with_device_state(uintptr_t snapshot, mybot_device_state_t state) {
    return (snapshot & ~DEVICE_MASK) | ((uintptr_t)state << DEVICE_SHIFT);
}

static bool transition_phase(mybot_state_model_t *model, mybot_runtime_phase_t expected,
                             mybot_runtime_phase_t desired, bool online) {
    if (!model) {
        return false;
    }
    uintptr_t current = (uintptr_t)aosl_atomic_read(&model->snapshot);
    if (snapshot_phase(current) != (uintptr_t)expected) {
        return false;
    }
    aosl_atomic_set(&model->snapshot, (intptr_t)with_online(with_phase(current, desired), online));
    return true;
}

void mybot_state_model_reset(mybot_state_model_t *model) {
    if (model) {
        aosl_atomic_set(&model->snapshot, MYBOT_PHASE_STOPPED);
    }
}

bool mybot_state_model_begin_start(mybot_state_model_t *model) {
    return transition_phase(model, MYBOT_PHASE_STOPPED, MYBOT_PHASE_WIFI_PROVISIONING, false);
}

bool mybot_state_model_begin_services(mybot_state_model_t *model) {
    return transition_phase(model, MYBOT_PHASE_WIFI_PROVISIONING, MYBOT_PHASE_STARTING_SERVICES,
                            true);
}

bool mybot_state_model_services_ready(mybot_state_model_t *model) {
    return transition_phase(model, MYBOT_PHASE_STARTING_SERVICES, MYBOT_PHASE_RUNNING, true);
}

static bool set_network(mybot_state_model_t *model, bool online) {
    if (!model) {
        return false;
    }
    uintptr_t current = (uintptr_t)aosl_atomic_read(&model->snapshot);
    if (snapshot_phase(current) != MYBOT_PHASE_RUNNING) {
        return false;
    }
    uintptr_t next = with_online(current, online);
    if (snapshot_device_state(next) == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        next = with_device_state(next, MYBOT_DEVICE_STATE_RUNTIME);
    }
    aosl_atomic_set(&model->snapshot, (intptr_t)next);
    return true;
}

bool mybot_state_model_network_lost(mybot_state_model_t *model) {
    return set_network(model, false);
}

bool mybot_state_model_network_restored(mybot_state_model_t *model) {
    return set_network(model, true);
}

bool mybot_state_model_set_device_state(mybot_state_model_t *model,
                                        mybot_device_state_t device_state) {
    if (!model || device_state < MYBOT_DEVICE_STATE_UNPROVISIONED ||
        device_state > MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        return false;
    }
    uintptr_t current = (uintptr_t)aosl_atomic_read(&model->snapshot);
    aosl_atomic_set(&model->snapshot, (intptr_t)with_device_state(current, device_state));
    return true;
}

bool mybot_state_model_fail(mybot_state_model_t *model) {
    if (!model) {
        return false;
    }
    uintptr_t current = (uintptr_t)aosl_atomic_read(&model->snapshot);
    uintptr_t phase = snapshot_phase(current);
    if (phase == MYBOT_PHASE_STOPPED || phase == MYBOT_PHASE_STOPPING ||
        phase == MYBOT_PHASE_FAILED) {
        return false;
    }
    aosl_atomic_set(&model->snapshot,
                    (intptr_t)with_online(with_phase(current, MYBOT_PHASE_FAILED), false));
    return true;
}

void mybot_state_model_begin_stop(mybot_state_model_t *model) {
    if (!model) {
        return;
    }
    uintptr_t current = (uintptr_t)aosl_atomic_read(&model->snapshot);
    aosl_atomic_set(&model->snapshot,
                    (intptr_t)with_online(with_phase(current, MYBOT_PHASE_STOPPING), false));
}

static mybot_state_t project_app_state(uintptr_t snapshot) {
    switch ((mybot_runtime_phase_t)snapshot_phase(snapshot)) {
    case MYBOT_PHASE_STOPPED:
        return MYBOT_STATE_STOPPED;
    case MYBOT_PHASE_WIFI_PROVISIONING:
        return MYBOT_STATE_WIFI_PROVISIONING;
    case MYBOT_PHASE_STARTING_SERVICES:
        return MYBOT_STATE_STARTING_SERVICES;
    case MYBOT_PHASE_FAILED:
        return MYBOT_STATE_FAILED;
    case MYBOT_PHASE_STOPPING:
        return MYBOT_STATE_STOPPING;
    case MYBOT_PHASE_RUNNING:
        if (!snapshot_online(snapshot)) {
            return MYBOT_STATE_WIFI_DISCONNECTED;
        }
        return snapshot_device_state(snapshot) == MYBOT_DEVICE_STATE_IN_CONVERSATION
                   ? MYBOT_STATE_IN_CONVERSATION
                   : MYBOT_STATE_READY;
    }
    return MYBOT_STATE_FAILED;
}

mybot_state_view_t mybot_state_model_get_view(const mybot_state_model_t *model) {
    mybot_state_view_t view = {MYBOT_STATE_STOPPED, MYBOT_DEVICE_STATE_UNPROVISIONED};
    if (!model) {
        return view;
    }
    uintptr_t snapshot = (uintptr_t)aosl_atomic_read(&model->snapshot);
    view.app_state = project_app_state(snapshot);
    view.device_state = snapshot_device_state(snapshot);
    return view;
}
