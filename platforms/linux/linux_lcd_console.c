#include <mybot/platform/mybot_lcd.h>

#include "linux_backends.h"

#include "api/aosl_log.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ANSI_RESET "\033[0m"
#define ANSI_BRIGHT_RED "\033[1;91m"

typedef struct {
    bool color_enabled;
} linux_lcd_console_ctx_t;

static bool console_color_enabled(void) {
    if (getenv("NO_COLOR") != NULL) {
        return false;
    }

    const char *force_color = getenv("MYBOT_LCD_COLOR");
    if (force_color) {
        return strcmp(force_color, "0") != 0;
    }

    const char *term = getenv("TERM");
    bool dumb_terminal = term && strcmp(term, "dumb") == 0;
    return isatty(STDOUT_FILENO) && !dumb_terminal;
}

static const char *screen_label(mybot_lcd_screen_t screen) {
    switch (screen) {
    case MYBOT_LCD_SCREEN_STARTING:
        return "STARTING";
    case MYBOT_LCD_SCREEN_WIFI_PROVISIONING:
        return "WI-FI PROVISIONING";
    case MYBOT_LCD_SCREEN_WIFI_DISCONNECTED:
        return "WI-FI DISCONNECTED";
    case MYBOT_LCD_SCREEN_STARTING_SERVICES:
        return "STARTING SERVICES";
    case MYBOT_LCD_SCREEN_PAIRING:
        return "PAIRING";
    case MYBOT_LCD_SCREEN_PAIR_CODE:
        return "PAIR CODE";
    case MYBOT_LCD_SCREEN_READY:
        return "READY";
    case MYBOT_LCD_SCREEN_IN_CONVERSATION:
        return "IN CONVERSATION";
    case MYBOT_LCD_SCREEN_FAILED:
        return "FAILED";
    case MYBOT_LCD_SCREEN_STOPPING:
        return "STOPPING";
    case MYBOT_LCD_SCREEN_COUNT:
        break;
    }
    return "UNKNOWN";
}

static int lcd_console_init(void **out_ctx) {
    if (!out_ctx) {
        return -1;
    }

    linux_lcd_console_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return -1;
    }

    ctx->color_enabled = console_color_enabled();
    *out_ctx = ctx;
    return 0;
}

static int lcd_console_render(void *opaque, const mybot_lcd_content_t *content) {
    if (!opaque || !content) {
        return -1;
    }

    linux_lcd_console_ctx_t *ctx = opaque;
    const char *color = ctx->color_enabled ? ANSI_BRIGHT_RED : "";
    const char *reset = ctx->color_enabled ? ANSI_RESET : "";

    if (content->screen == MYBOT_LCD_SCREEN_PAIR_CODE) {
        AOSL_LOG_INF("[LCD] %s%s: %s%s", color, screen_label(content->screen), content->pair_code,
                     reset);
    } else {
        AOSL_LOG_INF("[LCD] %s%s%s", color, screen_label(content->screen), reset);
    }
    return 0;
}

static void lcd_console_destroy(void *opaque) {
    free(opaque);
}

static const mybot_lcd_ops_t s_lcd_console_ops = {
    .name = "console",
    .init = lcd_console_init,
    .render = lcd_console_render,
    .destroy = lcd_console_destroy,
};

int linux_lcd_platform_register_console(void) {
    int ret = mybot_lcd_register(&s_lcd_console_ops);
    if (ret < 0) {
        AOSL_LOG_ERR("LCD console platform registration failed");
    }
    return ret;
}
