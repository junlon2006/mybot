#include <hal/aosl_hal_log.h>
#include <stdio.h>
#include <stdarg.h>
#include "os/os_api.h"
#include "os/os_cpu.h"

static OS_MUTEX g_log_mutex;
static volatile int g_log_init = 0;

int aosl_hal_printf(const char *format, va_list args)
{
    if (!g_log_init) {
        OS_ENTER_CRITICAL();
        if (!g_log_init) {
            os_mutex_create(&g_log_mutex);
            g_log_init = 1;
        }
        OS_EXIT_CRITICAL();
    }

    os_mutex_pend(&g_log_mutex, 0);
    int ret = vprintf(format, args);
    os_mutex_post(&g_log_mutex);

    return ret;
}
