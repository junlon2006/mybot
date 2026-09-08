/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_lcd.h>

#include "mybot_lcd_internal.h"
#include "mybot_platform_registry.h"

#include <stddef.h>
#include <string.h>

static bool screen_is_valid(mybot_lcd_screen_t screen) {
    return screen >= MYBOT_LCD_SCREEN_STARTING && screen < MYBOT_LCD_SCREEN_COUNT;
}

static int render_content(mybot_lcd_t *lcd, const mybot_lcd_content_t *content) {
    if (!lcd || !content || !screen_is_valid(content->screen)) {
        return -1;
    }
    if (content->screen == MYBOT_LCD_SCREEN_PAIR_CODE &&
        (content->pair_code[0] == '\0' ||
         memchr(content->pair_code, '\0', sizeof(content->pair_code)) == NULL)) {
        return -1;
    }

    int ret = -1;
    if (lcd->active) {
        ret = lcd->ops->render(lcd->ctx, content);
    }
    return ret;
}

bool mybot_lcd_is_registered(void) {
    return mybot_platform_registry_get()->lcd != NULL;
}

int mybot_lcd_init(mybot_lcd_t *lcd) {
    if (!lcd || lcd->active || !mybot_platform_registry_get()->lcd) {
        return -1;
    }

    lcd->ops = mybot_platform_registry_get()->lcd;
    if (lcd->ops->init(&lcd->ctx) < 0) {
        lcd->ctx = NULL;
        return -1;
    }

    lcd->active = true;
    return 0;
}

int mybot_lcd_show_screen(mybot_lcd_t *lcd, mybot_lcd_screen_t screen) {
    if (!screen_is_valid(screen) || screen == MYBOT_LCD_SCREEN_PAIR_CODE) {
        return -1;
    }

    mybot_lcd_content_t content;
    memset(&content, 0, sizeof(content));
    content.screen = screen;
    return mybot_lcd_show_content(lcd, &content);
}

int mybot_lcd_show_content(mybot_lcd_t *lcd, const mybot_lcd_content_t *content) {
    if (!content) {
        return -1;
    }
    return render_content(lcd, content);
}

int mybot_lcd_show_pair_code(mybot_lcd_t *lcd, const char *pair_code) {
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
    return mybot_lcd_show_content(lcd, &content);
}

void mybot_lcd_deinit(mybot_lcd_t *lcd) {
    if (!lcd || !lcd->active) {
        return;
    }

    lcd->active = false;
    lcd->ops->destroy(lcd->ctx);
    lcd->ctx = NULL;
}
