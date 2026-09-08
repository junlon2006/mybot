/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_lcd.h>

#include "mybot_lcd_internal.h"
#include "platform_test.h"

#include "api/aosl.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static mybot_lcd_t s_lcd;
static int s_init_count;
static int s_render_count;
static int s_destroy_count;
static bool s_init_fails;
static mybot_lcd_content_t s_last_content;

static int fake_init(void **ctx) {
    s_init_count++;
    if (s_init_fails) {
        return -1;
    }
    *ctx = &s_last_content;
    return 0;
}

static int fake_render(void *ctx, const mybot_lcd_content_t *content) {
    assert(ctx == &s_last_content);
    s_last_content = *content;
    s_render_count++;
    return 0;
}

static void fake_destroy(void *ctx) {
    assert(ctx == &s_last_content);
    s_destroy_count++;
}

int main(void) {
    const mybot_lcd_ops_t fake_ops = {
        .init = fake_init,
        .render = fake_render,
        .destroy = fake_destroy,
    };

    aosl_ctor();
    assert(!mybot_lcd_is_registered());
    assert(mybot_lcd_init(&s_lcd) < 0);
    mybot_platform_descriptor_t descriptor = mybot_test_platform_descriptor();
    descriptor.lcd = &fake_ops;
    assert(mybot_platform_register(&descriptor) == 0);
    assert(mybot_lcd_is_registered());

    s_init_fails = true;
    assert(mybot_lcd_init(&s_lcd) < 0);
    s_init_fails = false;
    assert(mybot_lcd_init(&s_lcd) == 0);
    assert(s_init_count == 2);
    assert(mybot_lcd_init(&s_lcd) < 0);

    assert(mybot_lcd_show_screen(&s_lcd, MYBOT_LCD_SCREEN_WIFI_PROVISIONING) == 0);
    assert(s_render_count == 1);
    assert(s_last_content.screen == MYBOT_LCD_SCREEN_WIFI_PROVISIONING);
    assert(s_last_content.pair_code[0] == '\0');
    assert(s_last_content.indicators == MYBOT_LCD_INDICATOR_NONE);

    mybot_lcd_content_t content = {0};
    content.screen = MYBOT_LCD_SCREEN_IN_CONVERSATION;
    content.indicators = MYBOT_LCD_INDICATOR_VP_REGISTERED;
    assert(mybot_lcd_show_content(&s_lcd, &content) == 0);
    assert(s_render_count == 2);
    assert(s_last_content.screen == MYBOT_LCD_SCREEN_IN_CONVERSATION);
    assert(s_last_content.indicators == MYBOT_LCD_INDICATOR_VP_REGISTERED);
    assert(mybot_lcd_show_content(&s_lcd, NULL) < 0);

    assert(mybot_lcd_show_pair_code(&s_lcd, "123456") == 0);
    assert(s_render_count == 3);
    assert(s_last_content.screen == MYBOT_LCD_SCREEN_PAIR_CODE);
    assert(strcmp(s_last_content.pair_code, "123456") == 0);

    assert(mybot_lcd_show_screen(&s_lcd, MYBOT_LCD_SCREEN_PAIR_CODE) < 0);
    assert(mybot_lcd_show_screen(&s_lcd, MYBOT_LCD_SCREEN_COUNT) < 0);
    content.screen = MYBOT_LCD_SCREEN_PAIR_CODE;
    content.indicators = MYBOT_LCD_INDICATOR_NONE;
    assert(mybot_lcd_show_content(&s_lcd, &content) < 0);
    assert(mybot_lcd_show_pair_code(&s_lcd, NULL) < 0);
    assert(mybot_lcd_show_pair_code(&s_lcd, "") < 0);
    assert(mybot_lcd_show_pair_code(&s_lcd, "1234567890123456") < 0);
    assert(s_render_count == 3);

    mybot_lcd_deinit(&s_lcd);
    mybot_lcd_deinit(&s_lcd);
    assert(s_destroy_count == 1);
    assert(mybot_lcd_show_screen(&s_lcd, MYBOT_LCD_SCREEN_READY) < 0);

    aosl_dtor();
    return 0;
}
