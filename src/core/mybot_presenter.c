/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_presenter.h"

#include <api/aosl_log.h>

#include <string.h>

int mybot_presenter_init(mybot_presenter_t *presenter) {
    if (!presenter) {
        return -1;
    }
    presenter->vp_registered = false;
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
    if (!presenter) {
        return;
    }
    if (presenter->active) {
        mybot_lcd_deinit(&presenter->lcd);
        presenter->active = false;
    }
    presenter->vp_registered = false;
}

void mybot_presenter_show_screen(mybot_presenter_t *presenter, mybot_lcd_screen_t screen) {
    if (!presenter) {
        return;
    }
    if (screen != MYBOT_LCD_SCREEN_IN_CONVERSATION) {
        presenter->vp_registered = false;
    }
    if (presenter->active) {
        mybot_lcd_content_t content;
        memset(&content, 0, sizeof(content));
        content.screen = screen;
        if (screen == MYBOT_LCD_SCREEN_IN_CONVERSATION && presenter->vp_registered) {
            content.indicators = MYBOT_LCD_INDICATOR_VP_REGISTERED;
        }
        if (mybot_lcd_show_content(&presenter->lcd, &content) < 0) {
            AOSL_LOG_WRN("failed to render LCD screen %d", (int)screen);
        }
    }
}

void mybot_presenter_show_pair_code(mybot_presenter_t *presenter, const char *code) {
    if (!presenter) {
        return;
    }
    presenter->vp_registered = false;
    if (presenter->active && mybot_lcd_show_pair_code(&presenter->lcd, code) < 0) {
        AOSL_LOG_WRN("failed to render LCD pair code");
    }
}

void mybot_presenter_set_vp_registered(mybot_presenter_t *presenter, bool registered) {
    if (presenter) {
        presenter->vp_registered = registered;
    }
}

void mybot_presenter_render_state(mybot_presenter_t *presenter,
                                  const mybot_state_model_t *state_model) {
    if (!presenter || !state_model) {
        return;
    }
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
        presenter->vp_registered = false;
        mybot_presenter_show_screen(presenter, MYBOT_LCD_SCREEN_PAIRING);
        break;
    case MYBOT_DEVICE_STATE_AWAITING_CLAIM:
        presenter->vp_registered = false;
        break;
    case MYBOT_DEVICE_STATE_RUNTIME:
        presenter->vp_registered = false;
        mybot_presenter_show_screen(presenter, MYBOT_LCD_SCREEN_READY);
        break;
    case MYBOT_DEVICE_STATE_IN_CONVERSATION:
        mybot_presenter_show_screen(presenter, MYBOT_LCD_SCREEN_IN_CONVERSATION);
        break;
    }
}
