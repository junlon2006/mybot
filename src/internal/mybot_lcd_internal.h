/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_LCD_INTERNAL_H_
#define MYBOT_LCD_INTERNAL_H_

#include <mybot/platform/mybot_lcd.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const mybot_lcd_ops_t *ops;
    void *ctx;
    bool active;
} mybot_lcd_t;

/**
 * SDK-internal LCD facade. The public mybot/platform/mybot_lcd.h only exposes
 * the platform contract (ops table + mybot_platform_register()); the SDK core drives
 * the registered implementation through the functions below. All calls are serialized by the
 * application control owner.
 */

/** Return whether the current platform registered an LCD implementation. */
bool mybot_lcd_is_registered(void);

/** Initialize the registered LCD implementation. */
int mybot_lcd_init(mybot_lcd_t *lcd);

/** Render a workflow screen from the control owner. */
int mybot_lcd_show_screen(mybot_lcd_t *lcd, mybot_lcd_screen_t screen);

/** Render semantic LCD content from the control owner. */
int mybot_lcd_show_content(mybot_lcd_t *lcd, const mybot_lcd_content_t *content);

/** Render the pairing screen from the control owner. */
int mybot_lcd_show_pair_code(mybot_lcd_t *lcd, const char *pair_code);

/** Release the LCD implementation from the control owner. Idempotent. */
void mybot_lcd_deinit(mybot_lcd_t *lcd);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_LCD_INTERNAL_H_ */
