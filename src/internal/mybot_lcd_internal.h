/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_LCD_INTERNAL_H_
#define MYBOT_LCD_INTERNAL_H_

#include <mybot/platform/mybot_lcd.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * SDK-internal LCD facade. The public mybot/platform/mybot_lcd.h only exposes
 * the platform contract (ops table + mybot_lcd_register()); the SDK core drives
 * the registered implementation through the functions below.
 */

/** Return whether the current platform registered an LCD implementation. */
bool mybot_lcd_is_registered(void);

/** Initialize the registered LCD implementation. */
int mybot_lcd_init(void);

/** Render a workflow screen that does not require additional content. */
int mybot_lcd_show_screen(mybot_lcd_screen_t screen);

/** Render the pairing screen with a server-provided pairing code. */
int mybot_lcd_show_pair_code(const char *pair_code);

/** Release the LCD implementation. Call only after all render callers have stopped. Idempotent. */
void mybot_lcd_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_LCD_INTERNAL_H_ */
