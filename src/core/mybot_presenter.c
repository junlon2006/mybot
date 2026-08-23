/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_presenter.h"

#include <api/aosl_log.h>

int mybot_presenter_init(mybot_presenter_t *presenter) {
    if (!presenter) {
        return -1;
    }
    if (!mybot_lcd_is_registered()) {
        return 0;
    }
    if (mybot_lcd_init(&presenter->lcd) < 0) {
        return -1;
    }
    presenter->active = true;
    return 0;
}

void mybot_presenter_deinit(mybot_presenter_t *presenter) {
    if (!presenter || !presenter->active) {
        return;
    }
    mybot_lcd_deinit(&presenter->lcd);
    presenter->active = false;
}

void mybot_presenter_show_screen(mybot_presenter_t *presenter, mybot_lcd_screen_t screen) {
    if (presenter && presenter->active && mybot_lcd_show_screen(&presenter->lcd, screen) < 0) {
        AOSL_LOG_WRN("failed to render LCD screen %d", (int)screen);
    }
}

void mybot_presenter_show_pair_code(mybot_presenter_t *presenter, const char *code) {
    if (presenter && presenter->active && mybot_lcd_show_pair_code(&presenter->lcd, code) < 0) {
        AOSL_LOG_WRN("failed to render LCD pair code");
    }
}

void mybot_presenter_render_state(mybot_presenter_t *presenter,
                                  const mybot_state_model_t *state_model) {
    mybot_state_view_t state = mybot_state_model_get_view(state_model);
    mybot_device_state_t device_state = state.device_state;
    mybot_state_t app_state = state.app_state;
    if ((device_state == MYBOT_DEVICE_STATE_IN_CONVERSATION &&
         app_state != MYBOT_STATE_IN_CONVERSATION) ||
        (device_state != MYBOT_DEVICE_STATE_IN_CONVERSATION && app_state != MYBOT_STATE_READY)) {
        return;
    }

    switch (device_state) {
    case MYBOT_DEVICE_STATE_UNPROVISIONED:
    case MYBOT_DEVICE_STATE_PAIRING:
        mybot_presenter_show_screen(presenter, MYBOT_LCD_SCREEN_PAIRING);
        break;
    case MYBOT_DEVICE_STATE_AWAITING_CLAIM:
        break;
    case MYBOT_DEVICE_STATE_RUNTIME:
        mybot_presenter_show_screen(presenter, MYBOT_LCD_SCREEN_READY);
        break;
    case MYBOT_DEVICE_STATE_IN_CONVERSATION:
        mybot_presenter_show_screen(presenter, MYBOT_LCD_SCREEN_IN_CONVERSATION);
        break;
    }
}
