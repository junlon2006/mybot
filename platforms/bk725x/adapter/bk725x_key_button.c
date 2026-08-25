/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_key.h>

#include "bk725x_platform_adapters_internal.h"
#include "mybot_key_dispatcher.h"

#include "mybot_platform_log.h"

#define TAG "mybot_key"

typedef struct {
    mybot_key_event_handler_t emit;
    void *user_data;
} key_adapter_binding_t;

static key_adapter_binding_t s_binding;

static void key_action_bridge(mybot_key_action_t action, void *user_data) {
    key_adapter_binding_t *binding = user_data;
    mybot_key_event_t sdk_event;

    if (!binding || !binding->emit) {
        return;
    }

    switch (action) {
    case MYBOT_KEY_ACTION_VOLUME_UP:
        sdk_event = MYBOT_KEY_EVENT_VOLUME_UP;
        break;
    case MYBOT_KEY_ACTION_VOLUME_DOWN:
        sdk_event = MYBOT_KEY_EVENT_VOLUME_DOWN;
        break;
    case MYBOT_KEY_ACTION_CONVERSATION_START:
        sdk_event = MYBOT_KEY_EVENT_CONVERSATION_START;
        break;
    case MYBOT_KEY_ACTION_CONVERSATION_STOP:
        sdk_event = MYBOT_KEY_EVENT_CONVERSATION_STOP;
        break;
    default:
        return;
    }
    binding->emit(sdk_event, binding->user_data);
}

static int key_adapter_init(void **ctx, mybot_key_event_handler_t emit, void *user_data) {
    if (!ctx || !emit || s_binding.emit) {
        return -1;
    }

    s_binding.emit = emit;
    s_binding.user_data = user_data;
    if (mybot_key_dispatcher_subscribe(ctx, key_action_bridge, &s_binding) < 0) {
        s_binding = (key_adapter_binding_t){0};
        return -1;
    }
    return 0;
}

static void key_adapter_destroy(void *ctx) {
    if (mybot_key_dispatcher_unsubscribe(ctx) < 0) {
        MYBOT_LOGE(TAG, "failed to remove key subscription");
        return;
    }
    s_binding = (key_adapter_binding_t){0};
}

static const mybot_key_ops_t s_ops = {
    .init = key_adapter_init,
    .destroy = key_adapter_destroy,
};

const mybot_key_ops_t *bk725x_key_platform_ops_button(void) {
    return &s_ops;
}
