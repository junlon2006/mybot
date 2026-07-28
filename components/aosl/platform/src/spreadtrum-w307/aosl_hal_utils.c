#include <stdio.h>
#include <stdlib.h>

#include <hal/aosl_hal_time.h>

extern uint32_t TM_GetTotalSeconds (void);
int aosl_hal_get_uuid(char buf[], int buf_sz)
{
  uint32_t tick_now, ts_sec, s;
  uint32_t t1, t2;

  tick_now = (uint32_t)aosl_hal_get_tick_ms ();
  ts_sec = TM_GetTotalSeconds ();

  s = tick_now + ts_sec;

  srand (s);
  t1 = rand ();
  t2 = rand ();

  snprintf (buf, buf_sz, "%08x%08x%08x%08x", tick_now, ts_sec, t1, t2);

  return 0;
}

int aosl_hal_os_version (char buf [], int buf_sz)
{
  snprintf(buf, buf_sz, "%s", "spreadtrum-w307");
  return 0;
}
