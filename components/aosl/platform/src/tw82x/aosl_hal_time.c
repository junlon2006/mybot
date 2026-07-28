#include <sdk.h>

#include <hal/aosl_hal_time.h>

uint64_t aosl_hal_get_tick_ms (void)
{
	return os_mseconds();
}

uint64_t aosl_hal_get_time_ms (void)
{
	struct timeval tv;
	if (gettimeofday (&tv, NULL) < 0)
		return 0;

	return (((uint64_t)tv.tv_sec * (uint64_t)1000) + tv.tv_usec / 1000);
}

//yyyy-mm-dd hh:mm:ss.xxx
int aosl_hal_get_time_str(char *buf, int len)
{
	uint32_t now;

    if (NULL == buf || 0 == len) {
        return -1;
    }

    now = aosl_hal_get_time_ms() / 1000;
    snprintf(buf, len, "%u", now);
	return 0;
}

void aosl_hal_msleep(uint64_t ms)
{
	os_sleep_ms(ms);
}