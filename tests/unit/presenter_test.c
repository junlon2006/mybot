/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_presenter.h"
#include "platform_test.h"

#include <api/aosl.h>

#include <assert.h>
#include <string.h>

static int s_init_count;
static int s_destroy_count;
static int s_render_count;
static int s_init_result;
static int s_render_result;
static mybot_lcd_content_t s_last_content;

static int lcd_init(void **ctx) {
    *ctx = s_init_result == 0 ? &s_last_content : NULL;
    s_init_count++;
    return s_init_result;
}

static int lcd_render(void *ctx, const mybot_lcd_content_t *content) {
    assert(ctx == &s_last_content);
    s_last_content = *content;
    s_render_count++;
    return s_render_result;
}

static void lcd_destroy(void *ctx) {
    assert(ctx == &s_last_content);
    s_destroy_count++;
}

int main(void) {
    const mybot_lcd_ops_t ops = {
        .init = lcd_init,
        .render = lcd_render,
        .destroy = lcd_destroy,
    };
    mybot_presenter_t presenter = {0};
    mybot_state_model_t state_model;

    assert(mybot_presenter_init(NULL) < 0);
    mybot_presenter_deinit(NULL);
    mybot_presenter_show_screen(NULL, MYBOT_LCD_SCREEN_READY);
    mybot_presenter_show_pair_code(NULL, "1");
    mybot_presenter_set_vp_registered(NULL, true);
    mybot_presenter_update_server_indicator(NULL, MYBOT_LCD_INDICATOR_LISTENING, true);
    mybot_presenter_clear_server_indicators(NULL);
    mybot_presenter_render_state(NULL, NULL);

    aosl_ctor();
    /* Optional LCD absence still permits presenter state updates and cleanup. */
    assert(mybot_presenter_init(&presenter) == 0);
    assert(!presenter.active);
    mybot_presenter_set_vp_registered(&presenter, true);
    mybot_presenter_show_screen(&presenter, MYBOT_LCD_SCREEN_IN_CONVERSATION);
    assert(presenter.indicators == MYBOT_LCD_INDICATOR_VP_REGISTERED);
    mybot_presenter_show_pair_code(&presenter, "123456");
    assert(presenter.indicators == MYBOT_LCD_INDICATOR_NONE);
    mybot_presenter_deinit(&presenter);
    assert(s_init_count == 0);
    assert(s_render_count == 0);
    assert(s_destroy_count == 0);

    mybot_platform_descriptor_t descriptor = mybot_test_platform_descriptor();
    descriptor.lcd = &ops;
    assert(mybot_platform_register(&descriptor) == 0);
    s_init_result = -1;
    assert(mybot_presenter_init(&presenter) == -1);
    assert(!presenter.active);
    assert(!presenter.lcd.active);
    assert(presenter.lcd.ctx == NULL);
    mybot_presenter_show_screen(&presenter, MYBOT_LCD_SCREEN_READY);
    mybot_presenter_deinit(&presenter);
    assert(s_render_count == 0);
    assert(s_destroy_count == 0);
    s_init_result = 0;
    assert(mybot_presenter_init(&presenter) == 0);
    assert(s_init_count == 2);

    mybot_presenter_show_screen(&presenter, MYBOT_LCD_SCREEN_STARTING);
    assert(s_last_content.screen == MYBOT_LCD_SCREEN_STARTING);

    int renders_before_invalid_screen = s_render_count;
    mybot_presenter_show_screen(&presenter, MYBOT_LCD_SCREEN_COUNT);
    assert(s_render_count == renders_before_invalid_screen);

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

    /* Pairing phases are exposed as a non-ready public state and still render
     * their semantic pairing screen.  Awaiting-claim keeps the pair-code
     * content already rendered by the lifecycle callback. */
    assert(mybot_state_model_set_device_state(&state_model, MYBOT_DEVICE_STATE_UNPROVISIONED));
    mybot_presenter_render_state(&presenter, &state_model);
    assert(s_last_content.screen == MYBOT_LCD_SCREEN_PAIRING);
    assert(mybot_state_model_set_device_state(&state_model, MYBOT_DEVICE_STATE_PAIRING));
    mybot_presenter_render_state(&presenter, &state_model);
    assert(s_last_content.screen == MYBOT_LCD_SCREEN_PAIRING);
    int renders_before_awaiting_claim = s_render_count;
    assert(mybot_state_model_set_device_state(&state_model, MYBOT_DEVICE_STATE_AWAITING_CLAIM));
    mybot_presenter_render_state(&presenter, &state_model);
    assert(s_render_count == renders_before_awaiting_claim);
    assert(mybot_state_model_set_device_state(&state_model, MYBOT_DEVICE_STATE_RUNTIME));
    mybot_presenter_render_state(&presenter, &state_model);
    assert(s_last_content.screen == MYBOT_LCD_SCREEN_READY);

    assert(mybot_state_model_set_device_state(&state_model, MYBOT_DEVICE_STATE_IN_CONVERSATION));
    mybot_presenter_render_state(&presenter, &state_model);
    assert(s_last_content.screen == MYBOT_LCD_SCREEN_IN_CONVERSATION);
    assert(s_last_content.indicators == MYBOT_LCD_INDICATOR_NONE);

    mybot_presenter_set_vp_registered(&presenter, true);
    mybot_presenter_show_screen(&presenter, MYBOT_LCD_SCREEN_IN_CONVERSATION);
    assert(s_last_content.screen == MYBOT_LCD_SCREEN_IN_CONVERSATION);
    assert(s_last_content.indicators == MYBOT_LCD_INDICATOR_VP_REGISTERED);

    mybot_presenter_update_server_indicator(&presenter, MYBOT_LCD_INDICATOR_THINKING, true);
    mybot_presenter_show_screen(&presenter, MYBOT_LCD_SCREEN_IN_CONVERSATION);
    assert(s_last_content.indicators ==
           (MYBOT_LCD_INDICATOR_VP_REGISTERED | MYBOT_LCD_INDICATOR_THINKING));
    mybot_presenter_update_server_indicator(&presenter, MYBOT_LCD_INDICATOR_SPEAKING, true);
    mybot_presenter_show_screen(&presenter, MYBOT_LCD_SCREEN_IN_CONVERSATION);
    assert(s_last_content.indicators ==
           (MYBOT_LCD_INDICATOR_VP_REGISTERED | MYBOT_LCD_INDICATOR_SPEAKING));
    mybot_presenter_update_server_indicator(&presenter, MYBOT_LCD_INDICATOR_SPEAKING, false);
    mybot_presenter_show_screen(&presenter, MYBOT_LCD_SCREEN_IN_CONVERSATION);
    assert(s_last_content.indicators == MYBOT_LCD_INDICATOR_VP_REGISTERED);
    mybot_presenter_update_server_indicator(&presenter, MYBOT_LCD_INDICATOR_LISTENING, true);
    mybot_presenter_clear_server_indicators(&presenter);
    mybot_presenter_show_screen(&presenter, MYBOT_LCD_SCREEN_IN_CONVERSATION);
    assert(s_last_content.indicators == MYBOT_LCD_INDICATOR_VP_REGISTERED);

    mybot_presenter_update_server_indicator(&presenter, MYBOT_LCD_INDICATOR_NONE, true);
    mybot_presenter_update_server_indicator(&presenter, MYBOT_LCD_INDICATOR_VP_REGISTERED, false);
    assert(presenter.indicators == MYBOT_LCD_INDICATOR_VP_REGISTERED);
    mybot_presenter_set_vp_registered(&presenter, false);
    assert(presenter.indicators == MYBOT_LCD_INDICATOR_NONE);

    /* Render failures leave the presenter available for the next update. */
    int renders_before_failure = s_render_count;
    s_render_result = -1;
    mybot_presenter_set_vp_registered(&presenter, true);
    mybot_presenter_show_screen(&presenter, MYBOT_LCD_SCREEN_IN_CONVERSATION);
    assert(presenter.active);
    assert(presenter.indicators == MYBOT_LCD_INDICATOR_VP_REGISTERED);
    mybot_presenter_show_pair_code(&presenter, "654321");
    assert(presenter.active);
    assert(presenter.indicators == MYBOT_LCD_INDICATOR_NONE);
    assert(s_render_count == renders_before_failure + 2);
    s_render_result = 0;
    mybot_presenter_show_pair_code(&presenter, "123456");
    assert(strcmp(s_last_content.pair_code, "123456") == 0);
    mybot_presenter_render_state(&presenter, NULL);
    assert(s_render_count == renders_before_failure + 3);

    assert(mybot_state_model_set_device_state(&state_model, MYBOT_DEVICE_STATE_RUNTIME));
    mybot_presenter_render_state(&presenter, &state_model);
    assert(s_last_content.screen == MYBOT_LCD_SCREEN_READY);
    assert(s_last_content.indicators == MYBOT_LCD_INDICATOR_NONE);

    mybot_presenter_deinit(&presenter);
    mybot_presenter_deinit(&presenter);
    assert(s_destroy_count == 1);
    aosl_dtor();
    return 0;
}
