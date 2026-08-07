#include <hal/aosl_hal_time.h>
#include <os/os_api.h>
#include <stdio.h>

// Match header: uint64_t aosl_hal_get_time_ms (void);
uint64_t aosl_hal_get_time_ms(void)
{
    // os_time_get() returns ticks (1 tick = 10ms usually in JieLi SDK)
    return (uint64_t)os_time_get() * 10;
}

// Match header: uint64_t aosl_hal_get_tick_ms (void);
uint64_t aosl_hal_get_tick_ms(void)
{
    return aosl_hal_get_time_ms();
}

// Match header: void aosl_hal_msleep(uint64_t ms);
void aosl_hal_msleep(uint64_t ms)
{
    // os_time_dly takes ticks (10ms units)
    uint32_t ticks = (uint32_t)(ms / 10);
    if (ticks == 0 && ms > 0) ticks = 1;
    os_time_dly(ticks);
}

// Match header: int aosl_hal_get_time_str(char *buf, int len);
int aosl_hal_get_time_str(char *buf, int len)
{
    // If real-time clock is not set, use uptime formatted as HH:MM:SS.mmm
    uint64_t total_ms = aosl_hal_get_time_ms();
    uint32_t ms = (uint32_t)(total_ms % 1000);
    uint32_t total_sec = (uint32_t)(total_ms / 1000);
    uint32_t sec = total_sec % 60;
    uint32_t total_min = total_sec / 60;
    uint32_t min = total_min % 60;
    uint32_t hours = total_min / 60;

    snprintf(buf, len, "%02u:%02u:%02u.%03u", (unsigned int)hours, (unsigned int)min, (unsigned int)sec, (unsigned int)ms);
    return 0;
}
