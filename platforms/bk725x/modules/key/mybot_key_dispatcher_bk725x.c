/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_key_dispatcher.h"

#include "mybot_platform_log.h"
#include <os/mem.h>
#include <os/os.h>

#include <stdbool.h>

#define TAG "mybot_key_dispatch"

typedef struct {
    mybot_key_action_handler_t handler;
    void *user_data;
    bool destroying;
    unsigned int api_users;
    beken_semaphore_t users_drained;
} mybot_key_subscription_t;

static mybot_key_subscription_t *s_subscription;
static beken_mutex_t s_lock;

int mybot_key_dispatcher_prepare(void) {
    if (s_lock) {
        return 0;
    }
    if (rtos_init_mutex(&s_lock) != BK_OK) {
        MYBOT_LOGE(TAG, "lock initialization failed");
        return -1;
    }
    return 0;
}

int mybot_key_dispatcher_publish(mybot_key_action_t action) {
    mybot_key_subscription_t *subscription;
    mybot_key_action_handler_t handler;
    void *user_data;

    if (action < MYBOT_KEY_ACTION_VOLUME_UP || action > MYBOT_KEY_ACTION_CONVERSATION_STOP ||
        !s_lock || rtos_lock_mutex(&s_lock) != BK_OK) {
        return -1;
    }
    subscription = s_subscription;
    if (!subscription || subscription->destroying || !subscription->handler) {
        (void)rtos_unlock_mutex(&s_lock);
        MYBOT_LOGW(TAG, "action ignored without subscriber, action=%d", (int)action);
        return -1;
    }

    handler = subscription->handler;
    user_data = subscription->user_data;
    ++subscription->api_users;
    (void)rtos_unlock_mutex(&s_lock);

    handler(action, user_data);

    if (rtos_lock_mutex(&s_lock) != BK_OK) {
        MYBOT_LOGE(TAG, "failed to release callback reference");
        return -1;
    }
    if (--subscription->api_users == 0 && subscription->destroying) {
        rtos_set_semaphore(&subscription->users_drained);
    }
    (void)rtos_unlock_mutex(&s_lock);
    return 0;
}

int mybot_key_dispatcher_subscribe(void **out_subscription,
                                   mybot_key_action_handler_t handler, void *user_data) {
    mybot_key_subscription_t *subscription;

    if (!out_subscription || !handler || !s_lock) {
        return -1;
    }
    *out_subscription = NULL;

    subscription = psram_zalloc(sizeof(*subscription));
    if (!subscription) {
        MYBOT_LOGE(TAG, "subscription allocation failed");
        return -1;
    }
    if (rtos_init_semaphore(&subscription->users_drained, 1) != BK_OK) {
        MYBOT_LOGE(TAG, "drain semaphore initialization failed");
        psram_free(subscription);
        return -1;
    }
    subscription->handler = handler;
    subscription->user_data = user_data;

    if (rtos_lock_mutex(&s_lock) != BK_OK) {
        goto fail;
    }
    if (s_subscription) {
        (void)rtos_unlock_mutex(&s_lock);
        MYBOT_LOGE(TAG, "subscriber already active");
        goto fail;
    }
    s_subscription = subscription;
    *out_subscription = subscription;
    (void)rtos_unlock_mutex(&s_lock);
    MYBOT_LOGI(TAG, "subscriber ready");
    return 0;

fail:
    rtos_deinit_semaphore(&subscription->users_drained);
    psram_free(subscription);
    return -1;
}

int mybot_key_dispatcher_unsubscribe(void *opaque) {
    mybot_key_subscription_t *subscription = opaque;
    bool wait_for_users;

    if (!subscription || !s_lock || rtos_lock_mutex(&s_lock) != BK_OK) {
        MYBOT_LOGE(TAG, "failed to remove subscriber");
        return -1;
    }
    if (s_subscription == subscription) {
        s_subscription = NULL;
    }
    subscription->destroying = true;
    wait_for_users = subscription->api_users != 0;
    (void)rtos_unlock_mutex(&s_lock);

    if (wait_for_users) {
        rtos_get_semaphore(&subscription->users_drained, BEKEN_WAIT_FOREVER);
    }
    subscription->handler = NULL;
    subscription->user_data = NULL;
    rtos_deinit_semaphore(&subscription->users_drained);
    psram_free(subscription);
    MYBOT_LOGI(TAG, "subscriber removed");
    return 0;
}
