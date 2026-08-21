/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_connectivity.h"

#include "mybot_platform_log.h"
#include <os/mem.h>
#include <os/os.h>

#include <stdbool.h>

#define TAG "mybot_conn"

typedef struct {
    mybot_connectivity_handler_t handler;
    void *user_data;
    bool last_event_valid;
    mybot_connectivity_event_t last_event;
    bool destroying;
    unsigned int api_users;
    beken_semaphore_t users_drained;
} mybot_connectivity_subscription_t;

static bool s_connected;
static mybot_connectivity_subscription_t *s_subscription;
static beken_mutex_t s_state_lock;
static beken_mutex_t s_dispatch_lock;

int mybot_connectivity_prepare(void) {
    bool state_lock_created = false;

    if (!s_state_lock) {
        if (rtos_init_mutex(&s_state_lock) != BK_OK) {
            MYBOT_LOGE(TAG, "state lock initialization failed");
            return -1;
        }
        state_lock_created = true;
    }
    if (!s_dispatch_lock && rtos_init_mutex(&s_dispatch_lock) != BK_OK) {
        MYBOT_LOGE(TAG, "dispatch lock initialization failed");
        if (state_lock_created) {
            rtos_deinit_mutex(&s_state_lock);
            s_state_lock = NULL;
        }
        return -1;
    }
    return 0;
}

int mybot_connectivity_publish(mybot_connectivity_event_t event) {
    mybot_connectivity_subscription_t *subscription;
    mybot_connectivity_handler_t handler;
    void *user_data;

    if (event < MYBOT_CONNECTIVITY_CONNECTED || event > MYBOT_CONNECTIVITY_FAILED ||
        !s_dispatch_lock || rtos_lock_mutex(&s_dispatch_lock) != BK_OK) {
        MYBOT_LOGE(TAG, "failed to serialize event=%d", (int)event);
        return -1;
    }
    if (!s_state_lock || rtos_lock_mutex(&s_state_lock) != BK_OK) {
        MYBOT_LOGE(TAG, "failed to lock state");
        (void)rtos_unlock_mutex(&s_dispatch_lock);
        return -1;
    }

    s_connected = event == MYBOT_CONNECTIVITY_CONNECTED;
    subscription = s_subscription;
    if (!subscription || subscription->destroying || !subscription->handler) {
        (void)rtos_unlock_mutex(&s_state_lock);
        (void)rtos_unlock_mutex(&s_dispatch_lock);
        MYBOT_LOGD(TAG, "event retained without subscriber, event=%d", (int)event);
        return 0;
    }
    if (subscription->last_event_valid && subscription->last_event == event) {
        (void)rtos_unlock_mutex(&s_state_lock);
        (void)rtos_unlock_mutex(&s_dispatch_lock);
        return 0;
    }

    subscription->last_event = event;
    subscription->last_event_valid = true;
    handler = subscription->handler;
    user_data = subscription->user_data;
    ++subscription->api_users;
    (void)rtos_unlock_mutex(&s_state_lock);

    MYBOT_LOGI(TAG, "event=%d", (int)event);
    handler(event, user_data);

    if (rtos_lock_mutex(&s_state_lock) != BK_OK) {
        MYBOT_LOGE(TAG, "failed to release callback reference");
        (void)rtos_unlock_mutex(&s_dispatch_lock);
        return -1;
    }
    if (--subscription->api_users == 0 && subscription->destroying) {
        rtos_set_semaphore(&subscription->users_drained);
    }
    (void)rtos_unlock_mutex(&s_state_lock);
    (void)rtos_unlock_mutex(&s_dispatch_lock);
    return 0;
}

int mybot_connectivity_subscribe(void **out_subscription,
                                 mybot_connectivity_handler_t handler, void *user_data) {
    mybot_connectivity_subscription_t *subscription;
    bool connected;

    if (!out_subscription || !handler || !s_state_lock || !s_dispatch_lock) {
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

    if (rtos_lock_mutex(&s_dispatch_lock) != BK_OK) {
        goto fail;
    }
    if (rtos_lock_mutex(&s_state_lock) != BK_OK) {
        (void)rtos_unlock_mutex(&s_dispatch_lock);
        goto fail;
    }
    if (s_subscription) {
        (void)rtos_unlock_mutex(&s_state_lock);
        (void)rtos_unlock_mutex(&s_dispatch_lock);
        MYBOT_LOGE(TAG, "subscriber already active");
        goto fail;
    }

    s_subscription = subscription;
    *out_subscription = subscription;
    connected = s_connected;
    if (connected) {
        subscription->last_event = MYBOT_CONNECTIVITY_CONNECTED;
        subscription->last_event_valid = true;
        ++subscription->api_users;
    }
    (void)rtos_unlock_mutex(&s_state_lock);

    if (connected) {
        handler(MYBOT_CONNECTIVITY_CONNECTED, user_data);
        if (rtos_lock_mutex(&s_state_lock) != BK_OK) {
            MYBOT_LOGE(TAG, "failed to release initial callback reference");
            (void)rtos_unlock_mutex(&s_dispatch_lock);
            return -1;
        }
        if (--subscription->api_users == 0 && subscription->destroying) {
            rtos_set_semaphore(&subscription->users_drained);
        }
        (void)rtos_unlock_mutex(&s_state_lock);
    }
    (void)rtos_unlock_mutex(&s_dispatch_lock);
    MYBOT_LOGI(TAG, "subscriber ready");
    return 0;

fail:
    rtos_deinit_semaphore(&subscription->users_drained);
    psram_free(subscription);
    return -1;
}

int mybot_connectivity_unsubscribe(void *opaque) {
    mybot_connectivity_subscription_t *subscription = opaque;
    bool wait_for_users;

    if (!subscription || !s_state_lock || rtos_lock_mutex(&s_state_lock) != BK_OK) {
        MYBOT_LOGE(TAG, "failed to remove subscriber");
        return -1;
    }
    if (s_subscription == subscription) {
        s_subscription = NULL;
    }
    subscription->destroying = true;
    wait_for_users = subscription->api_users != 0;
    (void)rtos_unlock_mutex(&s_state_lock);

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
