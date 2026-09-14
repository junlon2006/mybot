/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_presenter.h"

#include <api/aosl_log.h>

#include <string.h>

int mybot_presenter_init(mybot_presenter_t *presenter) {
    if (!presenter) {
        return -1;
    }
    presenter->indicators = MYBOT_LCD_INDICATOR_NONE;
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
    presenter->indicators = MYBOT_LCD_INDICATOR_NONE;
}

void mybot_presenter_show_screen(mybot_presenter_t *presenter, mybot_lcd_screen_t screen) {
    if (!presenter) {
        return;
    }
    if (screen != MYBOT_LCD_SCREEN_IN_CONVERSATION) {
        presenter->indicators = MYBOT_LCD_INDICATOR_NONE;
    }
    if (presenter->active) {
        mybot_lcd_content_t content;
        memset(&content, 0, sizeof(content));
        content.screen = screen;
        if (screen == MYBOT_LCD_SCREEN_IN_CONVERSATION) {
            content.indicators = presenter->indicators;
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
    presenter->indicators = MYBOT_LCD_INDICATOR_NONE;
    if (presenter->active && mybot_lcd_show_pair_code(&presenter->lcd, code) < 0) {
        AOSL_LOG_WRN("failed to render LCD pair code");
    }
}

void mybot_presenter_set_vp_registered(mybot_presenter_t *presenter, bool registered) {
    if (presenter) {
        if (registered) {
            presenter->indicators |= MYBOT_LCD_INDICATOR_VP_REGISTERED;
        } else {
            presenter->indicators &= ~MYBOT_LCD_INDICATOR_VP_REGISTERED;
        }
    }
}

void mybot_presenter_update_server_indicator(mybot_presenter_t *presenter,
                                             mybot_lcd_indicator_t indicator, bool active) {
    if (!presenter ||
        (indicator != MYBOT_LCD_INDICATOR_LISTENING && indicator != MYBOT_LCD_INDICATOR_THINKING &&
         indicator != MYBOT_LCD_INDICATOR_SPEAKING)) {
        return;
    }

    if (active) {
        presenter->indicators =
            (presenter->indicators & ~MYBOT_LCD_INDICATOR_SERVER_STATE_MASK) | indicator;
    } else {
        presenter->indicators &= ~indicator;
    }
}

void mybot_presenter_clear_server_indicators(mybot_presenter_t *presenter) {
    if (presenter) {
        presenter->indicators &= ~MYBOT_LCD_INDICATOR_SERVER_STATE_MASK;
    }
}

static bool app_state_matches_device_state(mybot_device_state_t device_state,
                                           mybot_state_t app_state) {
    switch (device_state) {
    case MYBOT_DEVICE_STATE_UNPROVISIONED:
    case MYBOT_DEVICE_STATE_PAIRING:
    case MYBOT_DEVICE_STATE_AWAITING_CLAIM:
        return app_state == MYBOT_STATE_PAIRING;
    case MYBOT_DEVICE_STATE_RUNTIME:
        return app_state == MYBOT_STATE_READY;
    case MYBOT_DEVICE_STATE_IN_CONVERSATION:
        return app_state == MYBOT_STATE_IN_CONVERSATION;
    default:
        return false;
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
    if (!app_state_matches_device_state(device_state, app_state)) {
        return;
    }

    switch (device_state) {
    case MYBOT_DEVICE_STATE_UNPROVISIONED:
    case MYBOT_DEVICE_STATE_PAIRING:
        presenter->indicators = MYBOT_LCD_INDICATOR_NONE;
        mybot_presenter_show_screen(presenter, MYBOT_LCD_SCREEN_PAIRING);
        break;
    case MYBOT_DEVICE_STATE_AWAITING_CLAIM:
        presenter->indicators = MYBOT_LCD_INDICATOR_NONE;
        break;
    case MYBOT_DEVICE_STATE_RUNTIME:
        presenter->indicators = MYBOT_LCD_INDICATOR_NONE;
        mybot_presenter_show_screen(presenter, MYBOT_LCD_SCREEN_READY);
        break;
    case MYBOT_DEVICE_STATE_IN_CONVERSATION:
        mybot_presenter_show_screen(presenter, MYBOT_LCD_SCREEN_IN_CONVERSATION);
        break;
    }
}
