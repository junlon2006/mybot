/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_lcd.h>

#include "mybot_lcd_internal.h"

#include "hal/aosl_hal_thread.h"

#include <stddef.h>
#include <string.h>

static const mybot_lcd_ops_t *s_ops;
static void *s_ctx;
static aosl_mutex_t s_render_lock;
static bool s_active;

static bool screen_is_valid(mybot_lcd_screen_t screen) {
    return screen >= MYBOT_LCD_SCREEN_STARTING && screen < MYBOT_LCD_SCREEN_COUNT;
}

static int render_content(const mybot_lcd_content_t *content) {
    if (!content || !screen_is_valid(content->screen) || !s_render_lock) {
        return -1;
    }

    if (aosl_hal_mutex_lock(s_render_lock) < 0) {
        return -1;
    }

    int ret = -1;
    if (s_active) {
        ret = s_ops->render(s_ctx, content);
    }

    if (aosl_hal_mutex_unlock(s_render_lock) < 0) {
        return -1;
    }
    return ret;
}

int mybot_lcd_register(const mybot_lcd_ops_t *ops) {
    if (!ops || !ops->init || !ops->render || !ops->destroy || s_active) {
        return -1;
    }
    s_ops = ops;
    return 0;
}

bool mybot_lcd_is_registered(void) {
    return s_ops != NULL;
}

int mybot_lcd_init(void) {
    if (s_active || !s_ops) {
        return -1;
    }

    s_render_lock = aosl_hal_mutex_create();
    if (!s_render_lock) {
        return -1;
    }

    if (s_ops->init(&s_ctx) < 0) {
        s_ctx = NULL;
        aosl_hal_mutex_destroy(s_render_lock);
        s_render_lock = NULL;
        return -1;
    }

    s_active = true;
    return 0;
}

int mybot_lcd_show_screen(mybot_lcd_screen_t screen) {
    if (!screen_is_valid(screen) || screen == MYBOT_LCD_SCREEN_PAIR_CODE) {
        return -1;
    }

    mybot_lcd_content_t content;
    memset(&content, 0, sizeof(content));
    content.screen = screen;
    return render_content(&content);
}

int mybot_lcd_show_pair_code(const char *pair_code) {
    if (!pair_code) {
        return -1;
    }

    size_t len = strlen(pair_code);
    if (len == 0 || len >= MYBOT_LCD_PAIR_CODE_CAPACITY) {
        return -1;
    }

    mybot_lcd_content_t content;
    memset(&content, 0, sizeof(content));
    content.screen = MYBOT_LCD_SCREEN_PAIR_CODE;
    memcpy(content.pair_code, pair_code, len + 1);
    return render_content(&content);
}

void mybot_lcd_deinit(void) {
    if (!s_active) {
        return;
    }

    aosl_hal_mutex_lock(s_render_lock);
    s_active = false;
    s_ops->destroy(s_ctx);
    s_ctx = NULL;
    aosl_hal_mutex_unlock(s_render_lock);

    aosl_hal_mutex_destroy(s_render_lock);
    s_render_lock = NULL;
}
