/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_button.h"

#include "mybot_event.h"

#include "mybot_platform_log.h"
#include <driver/gpio.h>
#include <gpio_driver.h>
#include <os/os.h>

#include <stdbool.h>
#include <stdint.h>

#define BUTTON_SCAN_THREAD_PRIORITY 2
#define BUTTON_SCAN_THREAD_STACK_SIZE 4096
#define BUTTON_COUNT 3
#define BUTTON_ACTIVE_LEVEL 0
#define BUTTON_SCAN_INTERVAL_MS 6
#define BUTTON_DEBOUNCE_TICKS 3
#define BUTTON_LONG_PRESS_TICKS (3000 / BUTTON_SCAN_INTERVAL_MS)

#define TAG "mybot_button"
#define LOGE(...) MYBOT_LOGE(TAG, ##__VA_ARGS__)
#define LOGW(...) MYBOT_LOGW(TAG, ##__VA_ARGS__)
#define LOGI(...) MYBOT_LOGI(TAG, ##__VA_ARGS__)

typedef enum {
    BUTTON_STATE_IDLE = 0,
    BUTTON_STATE_PRESSED,
    BUTTON_STATE_LONG_PRESSED,
} button_state_t;

typedef struct {
    gpio_id_t gpio;
    mybot_event_type_t release_event;
    mybot_event_type_t long_event;
    bool has_long_event;
    button_state_t state;
    uint16_t ticks;
    uint8_t stable_level;
    uint8_t debounce_count;
} button_item_t;

typedef struct {
    enum {
        BUTTON_LIFECYCLE_DOWN = 0,
        BUTTON_LIFECYCLE_READY,
        BUTTON_LIFECYCLE_STOPPING,
    } lifecycle;
    beken_semaphore_t scan_stop_requested;
    beken_thread_t scan_thread;
    button_item_t buttons[BUTTON_COUNT];
} button_manager_t;

static button_manager_t s_manager;
static beken_mutex_t s_manager_lock;

/* Application startup serializes the first call; this lock remains valid across restarts. */
static int ensure_manager_lock(void) {
    if (s_manager_lock) {
        return 0;
    }
    return rtos_init_mutex(&s_manager_lock) == BK_OK ? 0 : -1;
}

static const char *event_name(mybot_event_type_t event) {
    switch (event) {
    case MYBOT_EVENT_BUTTON_VOLUME_UP:
        return "volume_up";
    case MYBOT_EVENT_BUTTON_VOLUME_DOWN:
        return "volume_down";
    case MYBOT_EVENT_BUTTON_CONVERSATION_TOGGLE:
        return "conversation_toggle";
    case MYBOT_EVENT_BUTTON_PROVISIONING_REQUEST:
        return "provisioning_request";
    default:
        return "unknown";
    }
}

static void post_button_event(mybot_event_type_t event) {
    LOGI("event=%s", event_name(event));
    if (mybot_event_post(event) < 0) {
        LOGW("failed to post event=%s", event_name(event));
    }
}

static void scan_button(button_item_t *button) {
    uint8_t level = (uint8_t)bk_gpio_get_input(button->gpio);

    if (level != button->stable_level) {
        if (++button->debounce_count >= BUTTON_DEBOUNCE_TICKS) {
            button->stable_level = level;
            button->debounce_count = 0;
        }
    } else {
        button->debounce_count = 0;
    }

    if (button->state != BUTTON_STATE_IDLE && button->ticks < UINT16_MAX) {
        ++button->ticks;
    }

    switch (button->state) {
    case BUTTON_STATE_IDLE:
        if (button->stable_level == BUTTON_ACTIVE_LEVEL) {
            button->ticks = 0;
            button->state = BUTTON_STATE_PRESSED;
        }
        break;
    case BUTTON_STATE_PRESSED:
        if (button->stable_level != BUTTON_ACTIVE_LEVEL) {
            post_button_event(button->release_event);
            button->state = BUTTON_STATE_IDLE;
        } else if (button->has_long_event && button->ticks >= BUTTON_LONG_PRESS_TICKS) {
            post_button_event(button->long_event);
            button->state = BUTTON_STATE_LONG_PRESSED;
        }
        break;
    case BUTTON_STATE_LONG_PRESSED:
        if (button->stable_level != BUTTON_ACTIVE_LEVEL) {
            button->state = BUTTON_STATE_IDLE;
        }
        break;
    }
}

static int configure_gpio(gpio_id_t gpio) {
    gpio_dev_unmap(gpio);
    if (bk_gpio_disable_output(gpio) != BK_OK || bk_gpio_enable_input(gpio) != BK_OK ||
        bk_gpio_enable_pull(gpio) != BK_OK || bk_gpio_pull_up(gpio) != BK_OK) {
        LOGE("failed to configure GPIO%d", gpio);
        return -1;
    }
    return 0;
}

static void init_button(unsigned int index, gpio_id_t gpio,
                        mybot_event_type_t release_event, bool has_long_event,
                        mybot_event_type_t long_event) {
    button_item_t *button = &s_manager.buttons[index];

    button->gpio = gpio;
    button->release_event = release_event;
    button->has_long_event = has_long_event;
    button->long_event = long_event;
    button->stable_level = (uint8_t)bk_gpio_get_input(gpio);
}

static void scan_thread_main(beken_thread_arg_t arg) {
    (void)arg;

    for (;;) {
        if (rtos_get_semaphore(&s_manager.scan_stop_requested, BEKEN_NO_WAIT) == BK_OK) {
            break;
        }
        for (unsigned int i = 0; i < BUTTON_COUNT; ++i) {
            scan_button(&s_manager.buttons[i]);
        }
        rtos_delay_milliseconds(BUTTON_SCAN_INTERVAL_MS);
    }

    rtos_delete_thread(NULL);
}

static void release_resources(void) {
    if (s_manager.scan_stop_requested) {
        rtos_deinit_semaphore(&s_manager.scan_stop_requested);
    }
    s_manager = (button_manager_t){0};
}

int mybot_button_init(void) {
    if (ensure_manager_lock() < 0 || rtos_lock_mutex(&s_manager_lock) != BK_OK) {
        return -1;
    }
    if (s_manager.lifecycle == BUTTON_LIFECYCLE_READY) {
        rtos_unlock_mutex(&s_manager_lock);
        return 0;
    }
    if (s_manager.lifecycle != BUTTON_LIFECYCLE_DOWN) {
        rtos_unlock_mutex(&s_manager_lock);
        return -1;
    }
    if (rtos_init_semaphore(&s_manager.scan_stop_requested, 1) != BK_OK) {
        LOGE("failed to create RTOS resources");
        goto failed;
    }
    if (configure_gpio(GPIO_13) < 0 || configure_gpio(GPIO_8) < 0 ||
        configure_gpio(GPIO_12) < 0) {
        goto failed;
    }

    init_button(0, GPIO_13, MYBOT_EVENT_BUTTON_VOLUME_UP, false,
                MYBOT_EVENT_BUTTON_VOLUME_UP);
    init_button(1, GPIO_8, MYBOT_EVENT_BUTTON_VOLUME_DOWN, false,
                MYBOT_EVENT_BUTTON_VOLUME_DOWN);
    init_button(2, GPIO_12, MYBOT_EVENT_BUTTON_CONVERSATION_TOGGLE, true,
                MYBOT_EVENT_BUTTON_PROVISIONING_REQUEST);

    if (rtos_create_psram_thread(&s_manager.scan_thread, BUTTON_SCAN_THREAD_PRIORITY,
                                 "button_scan", scan_thread_main,
                                 BUTTON_SCAN_THREAD_STACK_SIZE, NULL) != BK_OK) {
        goto failed;
    }

    s_manager.lifecycle = BUTTON_LIFECYCLE_READY;
    LOGI("GPIO buttons ready: vol+=13, action=12, vol-=8");
    rtos_unlock_mutex(&s_manager_lock);
    return 0;

failed:
    release_resources();
    LOGE("failed to initialize GPIO buttons");
    rtos_unlock_mutex(&s_manager_lock);
    return -1;
}

void mybot_button_deinit(void) {
    if (!s_manager_lock || rtos_lock_mutex(&s_manager_lock) != BK_OK) {
        return;
    }
    if (s_manager.lifecycle != BUTTON_LIFECYCLE_READY) {
        rtos_unlock_mutex(&s_manager_lock);
        return;
    }

    s_manager.lifecycle = BUTTON_LIFECYCLE_STOPPING;
    rtos_unlock_mutex(&s_manager_lock);

    rtos_set_semaphore(&s_manager.scan_stop_requested);
    rtos_thread_join(&s_manager.scan_thread);

    if (rtos_lock_mutex(&s_manager_lock) != BK_OK) {
        return;
    }
    s_manager.scan_thread = NULL;
    release_resources();
    rtos_unlock_mutex(&s_manager_lock);
    LOGI("GPIO buttons stopped");
}
