/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_LCD_H_
#define MYBOT_LCD_H_

#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/** Maximum length of the pairing code buffer, including the NUL terminator. */
#define MYBOT_LCD_PAIR_CODE_CAPACITY 16

/**
 * Workflow screens rendered by the SDK.
 *
 * The LCD implementation receives these as semantic content and decides the concrete
 * layout, fonts, icons or QR-code presentation itself.
 */
typedef enum {
    /** The device is powering up. */
    MYBOT_LCD_SCREEN_STARTING = 0,
    /** The platform Wi-Fi provisioning/connection workflow is in progress. */
    MYBOT_LCD_SCREEN_WIFI_PROVISIONING,
    /** The Wi-Fi link was lost at runtime. */
    MYBOT_LCD_SCREEN_WIFI_DISCONNECTED,
    /** Wi-Fi is up and the remaining services are starting. */
    MYBOT_LCD_SCREEN_STARTING_SERVICES,
    /** The device is waiting for the pairing flow to complete. */
    MYBOT_LCD_SCREEN_PAIRING,
    /** The pairing screen with a server-provided pairing code. */
    MYBOT_LCD_SCREEN_PAIR_CODE,
    /** All services are up; the device is ready. */
    MYBOT_LCD_SCREEN_READY,
    /** A conversation is active. */
    MYBOT_LCD_SCREEN_IN_CONVERSATION,
    /** Startup failed unrecoverably. */
    MYBOT_LCD_SCREEN_FAILED,
    /** The application is shutting down. */
    MYBOT_LCD_SCREEN_STOPPING,
    /** Sentinel; not a valid screen value. */
    MYBOT_LCD_SCREEN_COUNT,
} mybot_lcd_screen_t;

/**
 * Semantic content passed to the LCD render() callback.
 */
typedef struct {
    /** The screen to render. */
    mybot_lcd_screen_t screen;
    /**
     * Pairing code; valid and NUL-terminated only when
     * screen == MYBOT_LCD_SCREEN_PAIR_CODE.
     */
    char pair_code[MYBOT_LCD_PAIR_CODE_CAPACITY];
} mybot_lcd_content_t;

/**
 * Platform LCD operations.
 *
 * render() receives semantic content so each platform can choose its own
 * layout, fonts, icons, or QR-code presentation. The content pointer is valid
 * only for the duration of the call and must not be retained by the implementation.
 *
 * @note render() is called by the application control owner. The SDK does not
 *       invoke render() concurrently.
 */
typedef struct {
    /**
     * Allocate and open the display.
     *
     * @param ctx [out] LCD implementation context handle
     * @return 0 on success, -1 on error
     */
    int (*init)(void **ctx);

    /**
     * Render one workflow screen.
     *
     * @param ctx     LCD implementation context from init()
     * @param content semantic screen content; borrowed for the duration of
     *                 the call
     * @return 0 on success, -1 on error
     */
    int (*render)(void *ctx, const mybot_lcd_content_t *content);

    /**
     * Release the display.
     *
     * Called by the application control owner after all render calls have stopped.
     *
     * @param ctx LCD implementation context from init()
     */
    void (*destroy)(void *ctx);
} mybot_lcd_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_LCD_H_ */
