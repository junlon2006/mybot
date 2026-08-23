/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_lcd.h>

#include "mybot_lcd_internal.h"

#include "api/aosl.h"
#include "api/aosl_atomic.h"
#include "hal/aosl_hal_time.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static mybot_lcd_t s_lcd;
static int s_init_count;
static int s_render_count;
static int s_destroy_count;
static bool s_init_fails;
static mybot_lcd_content_t s_last_content;
static aosl_atomic_t s_render_in_progress;
static aosl_atomic_t s_concurrent_render_detected;

typedef struct {
    mybot_lcd_screen_t screen;
    int result;
} render_thread_arg_t;

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
    if (aosl_atomic_xchg(&s_render_in_progress, true)) {
        aosl_atomic_set(&s_concurrent_render_detected, true);
    }
    aosl_hal_msleep(1);
    s_last_content = *content;
    s_render_count++;
    aosl_atomic_set(&s_render_in_progress, false);
    return 0;
}

static void fake_destroy(void *ctx) {
    assert(ctx == &s_last_content);
    s_destroy_count++;
}

static void *render_screens(void *opaque) {
    render_thread_arg_t *arg = opaque;
    for (int i = 0; i < 20; ++i) {
        if (mybot_lcd_show_screen(&s_lcd, arg->screen) < 0) {
            arg->result = -1;
            return NULL;
        }
    }
    return NULL;
}

int main(void) {
    const mybot_lcd_ops_t incomplete_ops = {0};
    const mybot_lcd_ops_t fake_ops = {
        .name = "fake",
        .init = fake_init,
        .render = fake_render,
        .destroy = fake_destroy,
    };

    aosl_ctor();
    assert(!mybot_lcd_is_registered());
    assert(mybot_lcd_register(NULL) < 0);
    assert(mybot_lcd_register(&incomplete_ops) < 0);
    assert(mybot_lcd_init(&s_lcd) < 0);
    assert(mybot_lcd_register(&fake_ops) == 0);
    assert(mybot_lcd_is_registered());

    s_init_fails = true;
    assert(mybot_lcd_init(&s_lcd) < 0);
    s_init_fails = false;
    assert(mybot_lcd_init(&s_lcd) == 0);
    assert(s_init_count == 2);
    assert(mybot_lcd_init(&s_lcd) < 0);
    assert(mybot_lcd_register(&fake_ops) < 0);

    assert(mybot_lcd_show_screen(&s_lcd, MYBOT_LCD_SCREEN_WIFI_PROVISIONING) == 0);
    assert(s_render_count == 1);
    assert(s_last_content.screen == MYBOT_LCD_SCREEN_WIFI_PROVISIONING);
    assert(s_last_content.pair_code[0] == '\0');

    assert(mybot_lcd_show_pair_code(&s_lcd, "123456") == 0);
    assert(s_render_count == 2);
    assert(s_last_content.screen == MYBOT_LCD_SCREEN_PAIR_CODE);
    assert(strcmp(s_last_content.pair_code, "123456") == 0);

    render_thread_arg_t first = {.screen = MYBOT_LCD_SCREEN_READY};
    render_thread_arg_t second = {.screen = MYBOT_LCD_SCREEN_IN_CONVERSATION};
    pthread_t first_thread;
    pthread_t second_thread;
    int rc = pthread_create(&first_thread, NULL, render_screens, &first);
    assert(rc == 0);
    rc = pthread_create(&second_thread, NULL, render_screens, &second);
    assert(rc == 0);
    assert(pthread_join(first_thread, NULL) == 0);
    assert(pthread_join(second_thread, NULL) == 0);
    assert(first.result == 0);
    assert(second.result == 0);
    assert(aosl_atomic_read(&s_concurrent_render_detected) == false);
    assert(s_render_count == 42);

    assert(mybot_lcd_show_screen(&s_lcd, MYBOT_LCD_SCREEN_PAIR_CODE) < 0);
    assert(mybot_lcd_show_screen(&s_lcd, MYBOT_LCD_SCREEN_COUNT) < 0);
    assert(mybot_lcd_show_pair_code(&s_lcd, NULL) < 0);
    assert(mybot_lcd_show_pair_code(&s_lcd, "") < 0);
    assert(mybot_lcd_show_pair_code(&s_lcd, "1234567890123456") < 0);
    assert(s_render_count == 42);

    mybot_lcd_deinit(&s_lcd);
    mybot_lcd_deinit(&s_lcd);
    assert(s_destroy_count == 1);
    assert(mybot_lcd_show_screen(&s_lcd, MYBOT_LCD_SCREEN_READY) < 0);

    aosl_dtor();
    return 0;
}
