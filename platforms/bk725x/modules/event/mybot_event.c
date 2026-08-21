/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_event.h"

#include "mybot_platform_log.h"
#include <os/os.h>

#define MYBOT_EVENT_QUEUE_DEPTH 32
#define MYBOT_EVENT_POST_TIMEOUT_MS 100

#define TAG "mybot_event"
#define LOGE(...) MYBOT_LOGE(TAG, ##__VA_ARGS__)
#define LOGW(...) MYBOT_LOGW(TAG, ##__VA_ARGS__)
#define LOGI(...) MYBOT_LOGI(TAG, ##__VA_ARGS__)

static beken_queue_t s_event_queue;

int mybot_event_init(void) {
    if (s_event_queue) {
        return 0;
    }

    if (rtos_init_queue(&s_event_queue, "mybot_event", sizeof(mybot_event_t),
                        MYBOT_EVENT_QUEUE_DEPTH) != BK_OK) {
        s_event_queue = NULL;
        LOGE("failed to create event queue");
        return -1;
    }

    LOGI("event queue ready, depth=%d", MYBOT_EVENT_QUEUE_DEPTH);
    return 0;
}

void mybot_event_deinit(void) {
    if (!s_event_queue) {
        return;
    }

    rtos_deinit_queue(&s_event_queue);
    s_event_queue = NULL;
    LOGI("event queue stopped");
}

int mybot_event_post_with_generation(mybot_event_type_t type, uint32_t source_generation) {
    mybot_event_t event = {
        .type = type,
        .source_generation = source_generation,
    };

    if (!s_event_queue || (unsigned int)type >= MYBOT_EVENT_TYPE_COUNT) {
        LOGE("invalid event post, type=%d", type);
        return -1;
    }

    if (rtos_push_to_queue(&s_event_queue, &event, MYBOT_EVENT_POST_TIMEOUT_MS) != BK_OK) {
        LOGE("failed to post event, type=%d", type);
        return -1;
    }
    return 0;
}

int mybot_event_post(mybot_event_type_t type) {
    return mybot_event_post_with_generation(type, 0);
}

int mybot_event_wait(mybot_event_t *event, uint32_t timeout_ms) {
    if (!event || !s_event_queue) {
        return -1;
    }

    return rtos_pop_from_queue(&s_event_queue, event, timeout_ms) == BK_OK ? 0 : -1;
}
