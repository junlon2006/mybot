/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_lcd.h>

#include "mybot_lcd_internal.h"

#include "linux_platform_adapters.h"

#include "api/aosl.h"
#include "api/aosl_log.h"

#include <assert.h>

int main(void) {
    mybot_lcd_t lcd = {0};
    aosl_ctor();
    aosl_set_log_level(AOSL_LOG_INFO);

    assert(linux_lcd_platform_register_console() == 0);
    assert(mybot_lcd_is_registered());
    assert(mybot_lcd_init(&lcd) == 0);

    for (int screen = MYBOT_LCD_SCREEN_STARTING; screen < MYBOT_LCD_SCREEN_COUNT; ++screen) {
        if (screen != MYBOT_LCD_SCREEN_PAIR_CODE) {
            assert(mybot_lcd_show_screen(&lcd, (mybot_lcd_screen_t)screen) == 0);
        }
    }
    assert(mybot_lcd_show_pair_code(&lcd, "123456") == 0);

    mybot_lcd_deinit(&lcd);
    aosl_dtor();
    return 0;
}
