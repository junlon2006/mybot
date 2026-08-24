/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_presenter.h"
#include "platform_test.h"

#include <api/aosl.h>

#include <assert.h>
#include <string.h>

static int s_init_count;
static int s_destroy_count;
static int s_render_count;
static mybot_lcd_content_t s_last_content;

static int lcd_init(void **ctx) {
    *ctx = &s_last_content;
    s_init_count++;
    return 0;
}

static int lcd_render(void *ctx, const mybot_lcd_content_t *content) {
    assert(ctx == &s_last_content);
    s_last_content = *content;
    s_render_count++;
    return 0;
}

static void lcd_destroy(void *ctx) {
    assert(ctx == &s_last_content);
    s_destroy_count++;
}

int main(void) {
    const mybot_lcd_ops_t ops = {
        .name = "presenter-test",
        .init = lcd_init,
        .render = lcd_render,
        .destroy = lcd_destroy,
    };
    mybot_presenter_t presenter = {0};
    mybot_state_model_t state_model;

    aosl_ctor();
    mybot_platform_descriptor_t descriptor = mybot_test_platform_descriptor();
    descriptor.capabilities |= MYBOT_PLATFORM_CAP_LCD;
    descriptor.lcd = &ops;
    assert(mybot_platform_register(&descriptor) == 0);
    assert(mybot_presenter_init(&presenter) == 0);
    assert(s_init_count == 1);

    mybot_presenter_show_screen(&presenter, MYBOT_LCD_SCREEN_STARTING);
    assert(s_last_content.screen == MYBOT_LCD_SCREEN_STARTING);

    mybot_presenter_show_pair_code(&presenter, "123456");
    assert(s_last_content.screen == MYBOT_LCD_SCREEN_PAIR_CODE);
    assert(strcmp(s_last_content.pair_code, "123456") == 0);

    mybot_state_model_reset(&state_model);
    assert(mybot_state_model_begin_start(&state_model));
    assert(mybot_state_model_begin_services(&state_model));
    assert(mybot_state_model_set_device_state(&state_model, MYBOT_DEVICE_STATE_IN_CONVERSATION));
    int renders_before_startup = s_render_count;
    mybot_presenter_render_state(&presenter, &state_model);
    assert(s_render_count == renders_before_startup);

    assert(mybot_state_model_set_device_state(&state_model, MYBOT_DEVICE_STATE_RUNTIME));
    assert(mybot_state_model_services_ready(&state_model));
    mybot_presenter_render_state(&presenter, &state_model);
    assert(s_last_content.screen == MYBOT_LCD_SCREEN_READY);

    assert(mybot_state_model_set_device_state(&state_model, MYBOT_DEVICE_STATE_IN_CONVERSATION));
    mybot_presenter_render_state(&presenter, &state_model);
    assert(s_last_content.screen == MYBOT_LCD_SCREEN_IN_CONVERSATION);

    mybot_presenter_deinit(&presenter);
    mybot_presenter_deinit(&presenter);
    assert(s_destroy_count == 1);
    aosl_dtor();
    return 0;
}
