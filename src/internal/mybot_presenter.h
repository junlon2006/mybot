/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_PRESENTER_H_
#define MYBOT_PRESENTER_H_

#include "mybot_lcd_internal.h"
#include "mybot_state_model.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    mybot_lcd_t lcd;
    bool active;
    bool vp_registered;
} mybot_presenter_t;

int mybot_presenter_init(mybot_presenter_t *presenter);
void mybot_presenter_deinit(mybot_presenter_t *presenter);
void mybot_presenter_show_screen(mybot_presenter_t *presenter, mybot_lcd_screen_t screen);
void mybot_presenter_show_pair_code(mybot_presenter_t *presenter, const char *code);
/** Set the voice-print registration indicator for the active conversation. */
void mybot_presenter_set_vp_registered(mybot_presenter_t *presenter, bool registered);
void mybot_presenter_render_state(mybot_presenter_t *presenter,
                                  const mybot_state_model_t *state_model);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_PRESENTER_H_ */
