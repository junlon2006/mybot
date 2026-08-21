/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_lcd.h>

#include "bk725x_platform_adapters_internal.h"

#include <mybot_display.h>

#include "mybot_platform_log.h"

#define TAG "mybot_lcd"

static int s_lcd_context;

static int lcd_dual_screen_init(void **out_ctx) {
    if (!out_ctx || !mybot_display_is_ready()) {
        MYBOT_LOGE(TAG, "init failed: display not ready or invalid context");
        return -1;
    }
    *out_ctx = &s_lcd_context;
    MYBOT_LOGI(TAG, "initialized");
    return 0;
}

static int lcd_dual_screen_render(void *ctx, const mybot_lcd_content_t *content) {
    mybot_display_screen_t screen;

    if (ctx != &s_lcd_context || !content) {
        MYBOT_LOGE(TAG, "render failed: invalid context or content");
        return -1;
    }
    if (content->screen == MYBOT_LCD_SCREEN_PAIR_CODE) {
        int result = mybot_display_show_pair_code(content->pair_code);
        if (result < 0) {
            MYBOT_LOGE(TAG, "pair-code render failed");
        }
        return result;
    }

    switch (content->screen) {
    case MYBOT_LCD_SCREEN_STARTING:
        screen = MYBOT_DISPLAY_SCREEN_STARTING;
        break;
    case MYBOT_LCD_SCREEN_WIFI_PROVISIONING:
        screen = MYBOT_DISPLAY_SCREEN_WIFI_PROVISIONING;
        break;
    case MYBOT_LCD_SCREEN_WIFI_DISCONNECTED:
        screen = MYBOT_DISPLAY_SCREEN_WIFI_DISCONNECTED;
        break;
    case MYBOT_LCD_SCREEN_STARTING_SERVICES:
        screen = MYBOT_DISPLAY_SCREEN_STARTING_SERVICES;
        break;
    case MYBOT_LCD_SCREEN_PAIRING:
        screen = MYBOT_DISPLAY_SCREEN_PAIRING;
        break;
    case MYBOT_LCD_SCREEN_READY:
        screen = MYBOT_DISPLAY_SCREEN_READY;
        break;
    case MYBOT_LCD_SCREEN_IN_CONVERSATION:
        screen = MYBOT_DISPLAY_SCREEN_IN_CONVERSATION;
        break;
    case MYBOT_LCD_SCREEN_FAILED:
        screen = MYBOT_DISPLAY_SCREEN_FAILED;
        break;
    case MYBOT_LCD_SCREEN_STOPPING:
        screen = MYBOT_DISPLAY_SCREEN_STOPPING;
        break;
    case MYBOT_LCD_SCREEN_PAIR_CODE:
    case MYBOT_LCD_SCREEN_COUNT:
    default:
        return -1;
    }
    int result = mybot_display_show_screen(screen);
    if (result < 0) {
        MYBOT_LOGE(TAG, "render failed, screen=%d", (int)content->screen);
    }
    return result;
}

static void lcd_dual_screen_destroy(void *ctx) {
    (void)ctx;
    /* The project owns the display lifetime across mybot stop/start cycles. */
    MYBOT_LOGI(TAG, "destroyed");
}

static const mybot_lcd_ops_t s_lcd_ops = {
    .name = "bk725x-dual-gc9d01",
    .init = lcd_dual_screen_init,
    .render = lcd_dual_screen_render,
    .destroy = lcd_dual_screen_destroy,
};

int bk725x_lcd_platform_register_dual_screen(void) {
    int result = mybot_lcd_register(&s_lcd_ops);
    if (result < 0) {
        MYBOT_LOGE(TAG, "registration failed");
    } else {
        MYBOT_LOGI(TAG, "backend ready: %s", s_lcd_ops.name);
    }
    return result;
}
