#include "lcd/mybot_lcd.h"

#include "api/aosl.h"
#include "api/aosl_log.h"

#include <assert.h>

void mybot_lcd_platform_register_console(void);

int main(void) {
    aosl_ctor();
    aosl_set_log_level(AOSL_LOG_INFO);

    mybot_lcd_platform_register_console();
    assert(mybot_lcd_is_registered());
    assert(mybot_lcd_init() == 0);

    for (int screen = MYBOT_LCD_SCREEN_STARTING; screen < MYBOT_LCD_SCREEN_COUNT; ++screen) {
        if (screen != MYBOT_LCD_SCREEN_PAIR_CODE) {
            assert(mybot_lcd_show_screen((mybot_lcd_screen_t)screen) == 0);
        }
    }
    assert(mybot_lcd_show_pair_code("123456") == 0);

    mybot_lcd_deinit();
    aosl_dtor();
    return 0;
}
