/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_LCD_H_
#define MYBOT_LCD_H_

#include <stdbool.h>
#include <mybot/mybot_export.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYBOT_LCD_PAIR_CODE_CAPACITY 16

typedef enum {
    MYBOT_LCD_SCREEN_STARTING = 0,
    MYBOT_LCD_SCREEN_WIFI_PROVISIONING,
    MYBOT_LCD_SCREEN_WIFI_DISCONNECTED,
    MYBOT_LCD_SCREEN_STARTING_SERVICES,
    MYBOT_LCD_SCREEN_PAIRING,
    MYBOT_LCD_SCREEN_PAIR_CODE,
    MYBOT_LCD_SCREEN_READY,
    MYBOT_LCD_SCREEN_IN_CONVERSATION,
    MYBOT_LCD_SCREEN_FAILED,
    MYBOT_LCD_SCREEN_STOPPING,
    MYBOT_LCD_SCREEN_COUNT,
} mybot_lcd_screen_t;

typedef struct {
    mybot_lcd_screen_t screen;
    char pair_code[MYBOT_LCD_PAIR_CODE_CAPACITY];
} mybot_lcd_content_t;

/**
 * Platform LCD operations. render() receives semantic content so each platform can choose its own
 * layout, fonts, icons, or QR-code presentation. The content pointer is valid only for the duration
 * of the call and must not be retained by the backend.
 */
typedef struct {
    const char *name;
    int (*init)(void **ctx);
    int (*render)(void *ctx, const mybot_lcd_content_t *content);
    void (*destroy)(void *ctx);
} mybot_lcd_ops_t;

/** Register the LCD backend for the current platform. Call before mybot_app_start(). */
MYBOT_API int mybot_lcd_register(const mybot_lcd_ops_t *ops);

/** Return whether the current platform registered an LCD backend. */
MYBOT_API bool mybot_lcd_is_registered(void);

/** Initialize the registered LCD backend. */
MYBOT_API int mybot_lcd_init(void);

/** Render a workflow screen that does not require additional content. */
MYBOT_API int mybot_lcd_show_screen(mybot_lcd_screen_t screen);

/** Render the pairing screen with a server-provided pairing code. */
MYBOT_API int mybot_lcd_show_pair_code(const char *pair_code);

/** Release the LCD backend. Call only after all render callers have stopped. Idempotent. */
MYBOT_API void mybot_lcd_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_LCD_H_ */
