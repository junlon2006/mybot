/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_display.h"

#include <components/bk_display.h>
#include "mybot_platform_log.h"
#include <driver/gpio.h>
#include <driver/pwr_clk.h>
#include <gpio_driver.h>
#include <lcd_panel_devices.h>
#include <modules/pm.h>
#include <os/mem.h>
#include <os/os.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DISPLAY_WIDTH 160
#define DISPLAY_HEIGHT 160
#define DISPLAY_COUNT 2
#define DISPLAY_BYTES_PER_PIXEL 2
#define DISPLAY_PANEL_SIZE (DISPLAY_WIDTH * DISPLAY_HEIGHT * DISPLAY_BYTES_PER_PIXEL)
#define DISPLAY_FRAME_SIZE (DISPLAY_PANEL_SIZE * DISPLAY_COUNT)

#define DISPLAY_BACKLIGHT_GPIO GPIO_25
#define DISPLAY_THREAD_PRIORITY 2
#define DISPLAY_THREAD_STACK_SIZE 4096
#define DISPLAY_QUEUE_DEPTH 4
#define DISPLAY_FLUSH_TIMEOUT_MS 5000

#define TAG "mybot_display"
#define LOGE(...) MYBOT_LOGE(TAG, ##__VA_ARGS__)
#define LOGW(...) MYBOT_LOGW(TAG, ##__VA_ARGS__)
#define LOGI(...) MYBOT_LOGI(TAG, ##__VA_ARGS__)

#define COLOR_BLACK 0x0000
#define COLOR_NEAR_BLACK 0x0841
#define COLOR_DIM_SEGMENT 0x18E3
#define COLOR_WHITE 0xFFFF
#define COLOR_CYAN 0x3FFF
#define COLOR_BLUE 0x4A7F
#define COLOR_GREEN 0x47E8
#define COLOR_YELLOW 0xFFE0
#define COLOR_AMBER 0xFD20
#define COLOR_RED 0xF986
#define COLOR_GRAY 0x8410

typedef enum {
    DISPLAY_COMMAND_SCREEN = 0,
    DISPLAY_COMMAND_PAIR_CODE,
    DISPLAY_COMMAND_STOP,
} display_command_type_t;

typedef struct {
    display_command_type_t type;
    mybot_display_screen_t screen;
    char pair_code[7];
    uint32_t trace_id;
} display_command_t;

typedef struct {
    frame_buffer_t frame;
    unsigned int slot;
    unsigned int panel;
    uint32_t trace_id;
} display_panel_frame_t;

typedef struct {
    bool power_owned;
    bool backlight_owned;
    beken_mutex_t api_lock;
    beken_mutex_t frame_lock;
    beken_queue_t command_queue;
    beken_semaphore_t command_done;
    beken_semaphore_t worker_stopped;
    beken_semaphore_t flush_done;
    beken_semaphore_t api_users_drained;
    beken_thread_t worker;
    bk_display_ctlr_handle_t controllers[DISPLAY_COUNT];
    bool controller_open[DISPLAY_COUNT];
    uint8_t *frame_pixels[2];
    display_panel_frame_t frames[2][DISPLAY_COUNT];
    bool panel_completed[2][DISPLAY_COUNT];
    unsigned int next_frame;
    uint32_t next_trace_id;
    int command_result;
} display_manager_t;

typedef enum {
    DISPLAY_LIFECYCLE_DOWN = 0,
    DISPLAY_LIFECYCLE_STARTING,
    DISPLAY_LIFECYCLE_READY,
    DISPLAY_LIFECYCLE_STOPPING,
} display_lifecycle_state_t;

static display_manager_t s_display;
static beken_mutex_t s_lifecycle_lock;
static display_lifecycle_state_t s_lifecycle;
static uint32_t s_api_users;

static bk_display_spi_ctlr_config_t s_spi_config[DISPLAY_COUNT] = {
    {
        .lcd_device = &lcd_device_gc9d01,
        .spi_id = 0,
        .dc_pin = GPIO_7,
        .reset_pin = GPIO_6,
        .te_pin = 0,
    },
    {
        .lcd_device = &lcd_device_gc9d01,
        .spi_id = 1,
        .dc_pin = GPIO_5,
        .reset_pin = GPIO_45,
        .te_pin = 0,
    },
};

static uint8_t *panel_pixels(unsigned int slot, unsigned int panel) {
    return s_display.frames[slot][panel].frame.frame;
}

static void put_pixel(uint8_t *pixels, int x, int y, uint16_t color) {
    if (x < 0 || x >= DISPLAY_WIDTH || y < 0 || y >= DISPLAY_HEIGHT) {
        return;
    }

    size_t offset = ((size_t)y * DISPLAY_WIDTH + (size_t)x) * DISPLAY_BYTES_PER_PIXEL;
    pixels[offset] = (uint8_t)(color >> 8);
    pixels[offset + 1] = (uint8_t)color;
}

static void fill_panel(uint8_t *pixels, uint16_t color) {
    uint8_t high = (uint8_t)(color >> 8);
    uint8_t low = (uint8_t)color;

    for (size_t offset = 0; offset < DISPLAY_PANEL_SIZE; offset += 2) {
        pixels[offset] = high;
        pixels[offset + 1] = low;
    }
}

static void fill_rect(uint8_t *pixels, int x, int y, int width, int height, uint16_t color) {
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            put_pixel(pixels, x + column, y + row, color);
        }
    }
}

static void draw_line(uint8_t *pixels, int x0, int y0, int x1, int y1, int thickness,
                      uint16_t color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;

    for (;;) {
        fill_rect(pixels, x0 - thickness / 2, y0 - thickness / 2, thickness, thickness,
                  color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int doubled = error * 2;
        if (doubled >= dy) {
            error += dy;
            x0 += sx;
        }
        if (doubled <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static void draw_ring(uint8_t *pixels, int radius, int thickness, uint16_t color) {
    const int center = DISPLAY_WIDTH / 2;
    int outer_squared = radius * radius;
    int inner_radius = radius - thickness;
    int inner_squared = inner_radius * inner_radius;

    for (int y = center - radius; y <= center + radius; ++y) {
        for (int x = center - radius; x <= center + radius; ++x) {
            int dx = x - center;
            int dy = y - center;
            int distance = dx * dx + dy * dy;
            if (distance <= outer_squared && distance >= inner_squared) {
                put_pixel(pixels, x, y, color);
            }
        }
    }
}

static uint16_t screen_color(mybot_display_screen_t screen) {
    switch (screen) {
    case MYBOT_DISPLAY_SCREEN_STARTING:
        return COLOR_CYAN;
    case MYBOT_DISPLAY_SCREEN_WIFI_PROVISIONING:
        return COLOR_AMBER;
    case MYBOT_DISPLAY_SCREEN_WIFI_DISCONNECTED:
        return COLOR_RED;
    case MYBOT_DISPLAY_SCREEN_STARTING_SERVICES:
        return COLOR_BLUE;
    case MYBOT_DISPLAY_SCREEN_PAIRING:
        return COLOR_YELLOW;
    case MYBOT_DISPLAY_SCREEN_READY:
        return COLOR_GREEN;
    case MYBOT_DISPLAY_SCREEN_IN_CONVERSATION:
        return COLOR_CYAN;
    case MYBOT_DISPLAY_SCREEN_FAILED:
        return COLOR_RED;
    case MYBOT_DISPLAY_SCREEN_STOPPING:
    case MYBOT_DISPLAY_SCREEN_COUNT:
        return COLOR_GRAY;
    }
    return COLOR_GRAY;
}

static void draw_state_icon(uint8_t *pixels, mybot_display_screen_t screen, uint16_t color) {
    switch (screen) {
    case MYBOT_DISPLAY_SCREEN_READY:
        draw_line(pixels, 52, 82, 71, 101, 8, color);
        draw_line(pixels, 70, 101, 111, 58, 8, color);
        break;
    case MYBOT_DISPLAY_SCREEN_FAILED:
    case MYBOT_DISPLAY_SCREEN_WIFI_DISCONNECTED:
        draw_line(pixels, 56, 56, 104, 104, 8, color);
        draw_line(pixels, 104, 56, 56, 104, 8, color);
        break;
    case MYBOT_DISPLAY_SCREEN_IN_CONVERSATION:
        fill_rect(pixels, 55, 65, 9, 30, color);
        fill_rect(pixels, 75, 53, 10, 54, color);
        fill_rect(pixels, 96, 65, 9, 30, color);
        break;
    case MYBOT_DISPLAY_SCREEN_WIFI_PROVISIONING:
    case MYBOT_DISPLAY_SCREEN_PAIRING:
        fill_rect(pixels, 50, 75, 14, 14, color);
        fill_rect(pixels, 73, 75, 14, 14, color);
        fill_rect(pixels, 96, 75, 14, 14, color);
        break;
    case MYBOT_DISPLAY_SCREEN_STARTING:
    case MYBOT_DISPLAY_SCREEN_STARTING_SERVICES:
        draw_line(pixels, 80, 51, 80, 80, 8, color);
        draw_line(pixels, 80, 80, 101, 95, 8, color);
        break;
    case MYBOT_DISPLAY_SCREEN_STOPPING:
    case MYBOT_DISPLAY_SCREEN_COUNT:
        fill_rect(pixels, 55, 76, 50, 8, color);
        break;
    }
}

static void draw_state_panel(uint8_t *pixels, mybot_display_screen_t screen) {
    uint16_t color = screen_color(screen);
    fill_panel(pixels, COLOR_NEAR_BLACK);
    draw_ring(pixels, 64, 5, color);
    draw_state_icon(pixels, screen, color);
}

static const uint8_t s_digit_segments[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
};

static void draw_digit(uint8_t *pixels, int x, int y, char digit) {
    const int width = 36;
    const int height = 84;
    const int thickness = 7;
    const int half = height / 2;
    uint8_t mask = s_digit_segments[digit - '0'];

#define DRAW_SEGMENT(bit, sx, sy, sw, sh)                                                     \
    fill_rect(pixels, (sx), (sy), (sw), (sh),                                                \
              (mask & (1u << (bit))) ? COLOR_WHITE : COLOR_DIM_SEGMENT)
    DRAW_SEGMENT(0, x + thickness, y, width - thickness * 2, thickness);
    DRAW_SEGMENT(1, x + width - thickness, y + thickness, thickness, half - thickness);
    DRAW_SEGMENT(2, x + width - thickness, y + half, thickness, half - thickness);
    DRAW_SEGMENT(3, x + thickness, y + height - thickness, width - thickness * 2, thickness);
    DRAW_SEGMENT(4, x, y + half, thickness, half - thickness);
    DRAW_SEGMENT(5, x, y + thickness, thickness, half - thickness);
    DRAW_SEGMENT(6, x + thickness, y + half - thickness / 2, width - thickness * 2, thickness);
#undef DRAW_SEGMENT
}

static void draw_pair_code_panel(uint8_t *pixels, const char *digits) {
    const int digit_width = 36;
    const int digit_gap = 7;
    const int group_width = digit_width * 3 + digit_gap * 2;
    const int start_x = (DISPLAY_WIDTH - group_width) / 2;

    fill_panel(pixels, COLOR_BLACK);
    draw_ring(pixels, 76, 2, COLOR_CYAN);
    for (int i = 0; i < 3; ++i) {
        draw_digit(pixels, start_x + i * (digit_width + digit_gap), 38, digits[i]);
    }
}

static void render_command(unsigned int slot, const display_command_t *command) {
    if (command->type == DISPLAY_COMMAND_PAIR_CODE) {
        draw_pair_code_panel(panel_pixels(slot, 0), command->pair_code);
        draw_pair_code_panel(panel_pixels(slot, 1), command->pair_code + 3);
        return;
    }

    draw_state_panel(panel_pixels(slot, 0), command->screen);
    draw_state_panel(panel_pixels(slot, 1), command->screen);
}

static bk_err_t flush_complete_callback(void *frame) {
    display_panel_frame_t *panel_frame = frame;
    uint32_t trace_id;

    if (!panel_frame || panel_frame->slot >= 2 || panel_frame->panel >= DISPLAY_COUNT) {
        return BK_FAIL;
    }
    if (rtos_lock_mutex(&s_display.frame_lock) != BK_OK) {
        return BK_FAIL;
    }
    trace_id = panel_frame->trace_id;
    s_display.panel_completed[panel_frame->slot][panel_frame->panel] = true;
    rtos_unlock_mutex(&s_display.frame_lock);
    LOGI("trace=%u flush complete: slot=%u LCD%u", (unsigned int)trace_id,
         panel_frame->slot, panel_frame->panel);
    rtos_set_semaphore(&s_display.flush_done);
    return BK_OK;
}

static bool frame_is_available(unsigned int index) {
    bool available = true;

    if (rtos_lock_mutex(&s_display.frame_lock) != BK_OK) {
        return false;
    }
    for (unsigned int panel = 0; panel < DISPLAY_COUNT; ++panel) {
        if (!s_display.panel_completed[index][panel]) {
            available = false;
            break;
        }
    }
    rtos_unlock_mutex(&s_display.frame_lock);
    return available;
}

static int wait_until_frame_available(unsigned int index) {
    uint32_t start = rtos_get_time();

    for (;;) {
        if (frame_is_available(index)) {
            return 0;
        }

        uint32_t elapsed = rtos_get_time() - start;
        if (elapsed >= DISPLAY_FLUSH_TIMEOUT_MS) {
            LOGE("timed out waiting for LCD frame %u", index);
            return -1;
        }

        if (rtos_get_semaphore(&s_display.flush_done,
                               DISPLAY_FLUSH_TIMEOUT_MS - elapsed) != BK_OK) {
            LOGE("timed out waiting for LCD frame %u", index);
            return -1;
        }
    }
}

static int submit_frame(unsigned int index, uint32_t trace_id) {
    int result = 0;

    if (rtos_lock_mutex(&s_display.frame_lock) != BK_OK) {
        return -1;
    }
    for (unsigned int panel = 0; panel < DISPLAY_COUNT; ++panel) {
        s_display.frames[index][panel].trace_id = trace_id;
        s_display.panel_completed[index][panel] = false;
    }
    rtos_unlock_mutex(&s_display.frame_lock);
    for (unsigned int panel = 0; panel < DISPLAY_COUNT; ++panel) {
        LOGI("trace=%u flush submit: slot=%u LCD%u", (unsigned int)trace_id, index,
             panel);
        if (bk_display_flush(s_display.controllers[panel],
                             &s_display.frames[index][panel].frame,
                             flush_complete_callback) != BK_OK) {
            LOGE("trace=%u failed to flush: slot=%u LCD%u",
                 (unsigned int)trace_id, index, panel);
            rtos_lock_mutex(&s_display.frame_lock);
            s_display.panel_completed[index][panel] = true;
            rtos_unlock_mutex(&s_display.frame_lock);
            result = -1;
        } else {
            LOGI("trace=%u flush accepted: slot=%u LCD%u",
                 (unsigned int)trace_id, index, panel);
        }
    }
    return result;
}

static int render_next_frame(const display_command_t *command) {
    unsigned int index = s_display.next_frame;
    int result;

    LOGI("trace=%u frame selected: slot=%u", (unsigned int)command->trace_id, index);
    if (wait_until_frame_available(index) < 0) {
        return -1;
    }
    render_command(index, command);
    LOGI("trace=%u render complete: slot=%u", (unsigned int)command->trace_id, index);
    result = submit_frame(index, command->trace_id);
    s_display.next_frame ^= 1u;
    return result;
}

static const char *screen_name(mybot_display_screen_t screen) {
    static const char *const names[MYBOT_DISPLAY_SCREEN_COUNT] = {
        [MYBOT_DISPLAY_SCREEN_STARTING] = "starting",
        [MYBOT_DISPLAY_SCREEN_WIFI_PROVISIONING] = "wifi_provisioning",
        [MYBOT_DISPLAY_SCREEN_WIFI_DISCONNECTED] = "wifi_disconnected",
        [MYBOT_DISPLAY_SCREEN_STARTING_SERVICES] = "starting_services",
        [MYBOT_DISPLAY_SCREEN_PAIRING] = "pairing",
        [MYBOT_DISPLAY_SCREEN_READY] = "ready",
        [MYBOT_DISPLAY_SCREEN_IN_CONVERSATION] = "in_conversation",
        [MYBOT_DISPLAY_SCREEN_FAILED] = "failed",
        [MYBOT_DISPLAY_SCREEN_STOPPING] = "stopping",
    };
    return screen >= MYBOT_DISPLAY_SCREEN_STARTING && screen < MYBOT_DISPLAY_SCREEN_COUNT
               ? names[screen]
               : "unknown";
}

static void display_worker_main(beken_thread_arg_t arg) {
    (void)arg;
    LOGI("worker started");

    for (;;) {
        display_command_t command;
        if (rtos_pop_from_queue(&s_display.command_queue, &command, BEKEN_WAIT_FOREVER) !=
            BK_OK) {
            continue;
        }
        if (command.type == DISPLAY_COMMAND_STOP) {
            LOGI("worker stop received");
            break;
        }

        if (command.type == DISPLAY_COMMAND_PAIR_CODE) {
            LOGI("trace=%u dequeued: state=pair_code",
                 (unsigned int)command.trace_id);
        } else {
            LOGI("trace=%u dequeued: state=%s", (unsigned int)command.trace_id,
                 screen_name(command.screen));
        }
        s_display.command_result = render_next_frame(&command);
        if (s_display.command_result < 0) {
            LOGE("trace=%u command failed, type=%d",
                 (unsigned int)command.trace_id, command.type);
        } else if (command.type == DISPLAY_COMMAND_PAIR_CODE) {
            LOGI("trace=%u render submitted: state=pair_code",
                 (unsigned int)command.trace_id);
        } else {
            LOGI("trace=%u render submitted: state=%s",
                 (unsigned int)command.trace_id, screen_name(command.screen));
        }
        rtos_set_semaphore(&s_display.command_done);
    }

    LOGI("worker stopped");
    rtos_set_semaphore(&s_display.worker_stopped);
    rtos_delete_thread(NULL);
}

static void backlight_close(void);

static int display_power_open(void) {
#if CONFIG_LDO3V3_ENABLE
    if (bk_pm_module_vote_ctrl_external_ldo(GPIO_CTRL_LDO_MODULE_LCD,
                                             CONFIG_LDO3V3_CTRL_GPIO,
                                             GPIO_OUTPUT_STATE_HIGH) != BK_OK) {
        LOGE("failed to enable display 3.3V supply on GPIO%d",
             CONFIG_LDO3V3_CTRL_GPIO);
        return -1;
    }
    s_display.power_owned = true;
    rtos_delay_milliseconds(10);
    LOGI("display 3.3V supply enabled on GPIO%d", CONFIG_LDO3V3_CTRL_GPIO);
#endif
    return 0;
}

static void display_power_close(void) {
#if CONFIG_LDO3V3_ENABLE
    if (!s_display.power_owned) {
        return;
    }
    if (bk_pm_module_vote_ctrl_external_ldo(GPIO_CTRL_LDO_MODULE_LCD,
                                             CONFIG_LDO3V3_CTRL_GPIO,
                                             GPIO_OUTPUT_STATE_LOW) != BK_OK) {
        LOGW("failed to release display 3.3V supply on GPIO%d",
             CONFIG_LDO3V3_CTRL_GPIO);
        return;
    }
    s_display.power_owned = false;
    LOGI("display 3.3V supply released");
#endif
}

static int backlight_open(void) {
    gpio_dev_unmap(DISPLAY_BACKLIGHT_GPIO);
    s_display.backlight_owned = true;
    if (bk_gpio_enable_output(DISPLAY_BACKLIGHT_GPIO) != BK_OK ||
        bk_gpio_enable_pull(DISPLAY_BACKLIGHT_GPIO) != BK_OK ||
        bk_gpio_pull_up(DISPLAY_BACKLIGHT_GPIO) != BK_OK ||
        bk_gpio_set_output_high(DISPLAY_BACKLIGHT_GPIO) != BK_OK) {
        LOGE("failed to enable LCD backlight");
        backlight_close();
        return -1;
    }
    LOGI("backlight enabled on GPIO%d", DISPLAY_BACKLIGHT_GPIO);
    return 0;
}

static void backlight_close(void) {
    if (!s_display.backlight_owned) {
        return;
    }
    bk_gpio_pull_down(DISPLAY_BACKLIGHT_GPIO);
    bk_gpio_set_output_low(DISPLAY_BACKLIGHT_GPIO);
    gpio_dev_unmap(DISPLAY_BACKLIGHT_GPIO);
    s_display.backlight_owned = false;
    LOGI("backlight disabled");
}

static int allocate_frames(void) {
    for (unsigned int slot = 0; slot < 2; ++slot) {
        s_display.frame_pixels[slot] = psram_malloc(DISPLAY_FRAME_SIZE);
        if (!s_display.frame_pixels[slot]) {
            return -1;
        }
        for (unsigned int panel = 0; panel < DISPLAY_COUNT; ++panel) {
            display_panel_frame_t *panel_frame = &s_display.frames[slot][panel];
            memset(panel_frame, 0, sizeof(*panel_frame));
            panel_frame->frame.frame =
                s_display.frame_pixels[slot] + panel * DISPLAY_PANEL_SIZE;
            panel_frame->frame.size = DISPLAY_PANEL_SIZE;
            panel_frame->frame.width = DISPLAY_WIDTH;
            panel_frame->frame.height = DISPLAY_HEIGHT;
            panel_frame->frame.fmt = PIXEL_FMT_RGB565;
            panel_frame->slot = slot;
            panel_frame->panel = panel;
            fill_panel(panel_frame->frame.frame, COLOR_BLACK);
        }
        for (unsigned int panel = 0; panel < DISPLAY_COUNT; ++panel) {
            s_display.panel_completed[slot][panel] = true;
        }
    }
    return 0;
}

static void free_frames(void) {
    for (unsigned int slot = 0; slot < 2; ++slot) {
        if (s_display.frame_pixels[slot]) {
            psram_free(s_display.frame_pixels[slot]);
            s_display.frame_pixels[slot] = NULL;
        }
    }
}

static void release_resources(void) {
    bool bus_owned[DISPLAY_COUNT];

    for (unsigned int panel = 0; panel < DISPLAY_COUNT; ++panel) {
        bus_owned[panel] = s_display.controllers[panel] != NULL;
    }
    for (unsigned int panel = 0; panel < DISPLAY_COUNT; ++panel) {
        if (s_display.controller_open[panel]) {
            bk_display_close(s_display.controllers[panel]);
            s_display.controller_open[panel] = false;

            for (unsigned int remaining = panel + 1; remaining < DISPLAY_COUNT;
                 ++remaining) {
                if (s_display.controller_open[remaining]) {
                    bk_pm_module_vote_psram_ctrl(PM_POWER_PSRAM_MODULE_NAME_VIDP_LCD,
                                                 PM_POWER_MODULE_STATE_ON);
                    bk_pm_module_vote_cpu_freq(PM_DEV_ID_DISP, PM_CPU_FRQ_480M);
                    break;
                }
            }
        }
        if (s_display.controllers[panel]) {
            bk_display_delete(s_display.controllers[panel]);
            s_display.controllers[panel] = NULL;
        }
    }
    if (bus_owned[0]) {
        gpio_dev_unmap(GPIO_22);
        gpio_dev_unmap(GPIO_23);
        gpio_dev_unmap(GPIO_24);
    }
    if (bus_owned[1]) {
        gpio_dev_unmap(GPIO_2);
        gpio_dev_unmap(GPIO_3);
        gpio_dev_unmap(GPIO_4);
    }
    backlight_close();
    free_frames();
    display_power_close();
    if (s_display.api_users_drained) {
        rtos_deinit_semaphore(&s_display.api_users_drained);
    }
    if (s_display.flush_done) {
        rtos_deinit_semaphore(&s_display.flush_done);
    }
    if (s_display.worker_stopped) {
        rtos_deinit_semaphore(&s_display.worker_stopped);
    }
    if (s_display.command_done) {
        rtos_deinit_semaphore(&s_display.command_done);
    }
    if (s_display.command_queue) {
        rtos_deinit_queue(&s_display.command_queue);
    }
    if (s_display.frame_lock) {
        rtos_deinit_mutex(&s_display.frame_lock);
    }
    if (s_display.api_lock) {
        rtos_deinit_mutex(&s_display.api_lock);
    }
    s_display = (display_manager_t){0};
}

/* The controller serializes first initialization. Keep this mutex alive across restarts. */
static int ensure_lifecycle_lock(void) {
    if (s_lifecycle_lock) {
        return 0;
    }
    return rtos_init_mutex(&s_lifecycle_lock) == BK_OK ? 0 : -1;
}

static bool display_api_acquire(void) {
    bool acquired = false;

    if (!s_lifecycle_lock || rtos_lock_mutex(&s_lifecycle_lock) != BK_OK) {
        return false;
    }
    if (s_lifecycle == DISPLAY_LIFECYCLE_READY) {
        ++s_api_users;
        acquired = true;
    }
    rtos_unlock_mutex(&s_lifecycle_lock);
    return acquired;
}

static void display_api_release(void) {
    bool drained = false;

    if (rtos_lock_mutex(&s_lifecycle_lock) != BK_OK) {
        return;
    }
    if (s_api_users > 0) {
        --s_api_users;
    }
    drained = s_lifecycle == DISPLAY_LIFECYCLE_STOPPING && s_api_users == 0;
    rtos_unlock_mutex(&s_lifecycle_lock);
    if (drained) {
        rtos_set_semaphore(&s_display.api_users_drained);
    }
}

int mybot_display_init(void) {
    if (ensure_lifecycle_lock() < 0 || rtos_lock_mutex(&s_lifecycle_lock) != BK_OK) {
        return -1;
    }
    if (s_lifecycle == DISPLAY_LIFECYCLE_READY) {
        rtos_unlock_mutex(&s_lifecycle_lock);
        return 0;
    }
    if (s_lifecycle != DISPLAY_LIFECYCLE_DOWN) {
        LOGW("initialization already in progress");
        rtos_unlock_mutex(&s_lifecycle_lock);
        return -1;
    }
    s_lifecycle = DISPLAY_LIFECYCLE_STARTING;
    rtos_unlock_mutex(&s_lifecycle_lock);

    LOGI("initializing dual GC9D01 display");
    if (display_power_open() < 0) {
        goto failed;
    }
    if (rtos_init_mutex(&s_display.api_lock) != BK_OK ||
        rtos_init_mutex(&s_display.frame_lock) != BK_OK ||
        rtos_init_semaphore(&s_display.command_done, 1) != BK_OK ||
        rtos_init_semaphore(&s_display.worker_stopped, 1) != BK_OK ||
        rtos_init_semaphore(&s_display.flush_done, DISPLAY_COUNT * 2) != BK_OK ||
        rtos_init_semaphore(&s_display.api_users_drained, 1) != BK_OK ||
        rtos_init_queue(&s_display.command_queue, "mybot_display", sizeof(display_command_t),
                        DISPLAY_QUEUE_DEPTH) != BK_OK) {
        LOGE("failed to create synchronization resources");
        goto failed;
    }
    LOGI("synchronization resources ready");
    if (allocate_frames() < 0) {
        LOGE("failed to allocate dual-screen buffers");
        goto failed;
    }
    LOGI("dual-screen PSRAM frames ready: slots=2 bytes=%u",
         (unsigned int)(DISPLAY_FRAME_SIZE * 2));
    for (unsigned int panel = 0; panel < DISPLAY_COUNT; ++panel) {
        if (bk_display_spi_new(&s_display.controllers[panel], &s_spi_config[panel]) != BK_OK) {
            LOGE("failed to create GC9D01 LCD%u", panel);
            goto failed;
        }
        if (bk_display_open(s_display.controllers[panel]) != BK_OK) {
            LOGE("failed to open GC9D01 LCD%u", panel);
            goto failed;
        }
        s_display.controller_open[panel] = true;
        LOGI("GC9D01 LCD%u opened", panel);
    }

    /* Prime both asynchronous SPI paths with black frames before enabling the backlight. */
    LOGI("priming dual display with black frames");
    if (submit_frame(0, 0) < 0 || submit_frame(1, 0) < 0 ||
        wait_until_frame_available(0) < 0 ||
        backlight_open() < 0) {
        LOGE("failed to prime dual display");
        goto failed;
    }
    s_display.next_frame = 0;
    LOGI("dual display primed");

    if (rtos_create_psram_thread(&s_display.worker, DISPLAY_THREAD_PRIORITY, "mybot_display",
                                 display_worker_main, DISPLAY_THREAD_STACK_SIZE, NULL) != BK_OK) {
        LOGE("failed to create display worker");
        goto failed;
    }
    LOGI("display worker created");
    rtos_lock_mutex(&s_lifecycle_lock);
    s_lifecycle = DISPLAY_LIFECYCLE_READY;
    rtos_unlock_mutex(&s_lifecycle_lock);
    LOGI("dual GC9D01 display ready");
    return 0;

failed:
    LOGE("dual display initialization failed");
    release_resources();
    rtos_lock_mutex(&s_lifecycle_lock);
    s_lifecycle = DISPLAY_LIFECYCLE_DOWN;
    rtos_unlock_mutex(&s_lifecycle_lock);
    return -1;
}

void mybot_display_deinit(void) {
    bool owner = false;
    bool wait_for_users = false;

    if (!s_lifecycle_lock) {
        return;
    }
    LOGI("deinit requested");

    while (!owner) {
        if (rtos_lock_mutex(&s_lifecycle_lock) != BK_OK) {
            return;
        }
        if (s_lifecycle == DISPLAY_LIFECYCLE_READY) {
            s_lifecycle = DISPLAY_LIFECYCLE_STOPPING;
            wait_for_users = s_api_users != 0;
            owner = true;
        }
        if (s_lifecycle == DISPLAY_LIFECYCLE_DOWN) {
            rtos_unlock_mutex(&s_lifecycle_lock);
            return;
        }
        rtos_unlock_mutex(&s_lifecycle_lock);
        if (!owner) {
            rtos_delay_milliseconds(1);
        }
    }

    if (wait_for_users) {
        LOGI("waiting for active display requests");
        rtos_get_semaphore(&s_display.api_users_drained, BEKEN_NEVER_TIMEOUT);
    }

    display_command_t stop = {.type = DISPLAY_COMMAND_STOP};
    LOGI("sending worker stop");
    rtos_push_to_queue_front(&s_display.command_queue, &stop, BEKEN_WAIT_FOREVER);
    rtos_get_semaphore(&s_display.worker_stopped, BEKEN_NEVER_TIMEOUT);
    rtos_thread_join(&s_display.worker);
    s_display.worker = NULL;

    LOGI("releasing display resources");
    release_resources();
    rtos_lock_mutex(&s_lifecycle_lock);
    s_api_users = 0;
    s_lifecycle = DISPLAY_LIFECYCLE_DOWN;
    rtos_unlock_mutex(&s_lifecycle_lock);
    LOGI("dual display deinitialized");
}

bool mybot_display_is_ready(void) {
    bool ready = false;

    if (!s_lifecycle_lock || rtos_lock_mutex(&s_lifecycle_lock) != BK_OK) {
        return false;
    }
    ready = s_lifecycle == DISPLAY_LIFECYCLE_READY;
    rtos_unlock_mutex(&s_lifecycle_lock);
    return ready;
}

static int send_command(const display_command_t *command) {
    display_command_t queued_command;
    int result = -1;

    if (!command || !display_api_acquire()) {
        return -1;
    }
    rtos_lock_mutex(&s_display.api_lock);
    queued_command = *command;
    ++s_display.next_trace_id;
    if (s_display.next_trace_id == 0) {
        ++s_display.next_trace_id;
    }
    queued_command.trace_id = s_display.next_trace_id;
    if (queued_command.type == DISPLAY_COMMAND_PAIR_CODE) {
        LOGI("trace=%u request: state=pair_code",
             (unsigned int)queued_command.trace_id);
    } else {
        LOGI("trace=%u request: state=%s", (unsigned int)queued_command.trace_id,
             screen_name(queued_command.screen));
    }
    if (rtos_push_to_queue(&s_display.command_queue, &queued_command, BEKEN_WAIT_FOREVER) ==
        BK_OK) {
        LOGI("trace=%u enqueued", (unsigned int)queued_command.trace_id);
        rtos_get_semaphore(&s_display.command_done, BEKEN_NEVER_TIMEOUT);
        result = s_display.command_result;
        if (queued_command.type == DISPLAY_COMMAND_PAIR_CODE) {
            LOGI("trace=%u request returned: state=pair_code result=%d",
                 (unsigned int)queued_command.trace_id, result);
        } else {
            LOGI("trace=%u request returned: state=%s result=%d",
                 (unsigned int)queued_command.trace_id,
                 screen_name(queued_command.screen), result);
        }
    } else {
        LOGE("trace=%u command transport failed, type=%d",
             (unsigned int)queued_command.trace_id, queued_command.type);
    }
    rtos_unlock_mutex(&s_display.api_lock);
    display_api_release();
    return result;
}

int mybot_display_show_screen(mybot_display_screen_t screen) {
    if (screen < MYBOT_DISPLAY_SCREEN_STARTING || screen >= MYBOT_DISPLAY_SCREEN_COUNT) {
        return -1;
    }

    display_command_t command = {
        .type = DISPLAY_COMMAND_SCREEN,
        .screen = screen,
    };
    return send_command(&command);
}

int mybot_display_show_pair_code(const char *pair_code) {
    if (!pair_code || strlen(pair_code) != 6) {
        return -1;
    }
    for (size_t i = 0; i < 6; ++i) {
        if (pair_code[i] < '0' || pair_code[i] > '9') {
            return -1;
        }
    }

    display_command_t command = {.type = DISPLAY_COMMAND_PAIR_CODE};
    memcpy(command.pair_code, pair_code, sizeof(command.pair_code));
    return send_command(&command);
}
