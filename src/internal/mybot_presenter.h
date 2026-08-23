/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_PRESENTER_H_
#define MYBOT_PRESENTER_H_

#include "mybot_device_lifecycle.h"
#include "mybot_lcd_internal.h"

#include <mybot/mybot.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    mybot_lcd_t lcd;
    bool active;
} mybot_presenter_t;

int mybot_presenter_init(mybot_presenter_t *presenter);
void mybot_presenter_deinit(mybot_presenter_t *presenter);
void mybot_presenter_show_screen(mybot_presenter_t *presenter, mybot_lcd_screen_t screen);
void mybot_presenter_show_pair_code(mybot_presenter_t *presenter, const char *code);
void mybot_presenter_render_device_state(mybot_presenter_t *presenter,
                                         mybot_device_state_t device_state,
                                         mybot_state_t app_state);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_PRESENTER_H_ */
