#include <stdbool.h>
#include <hal/aosl_hal_atomic.h>

/*
 * ARMCC RVCT implementation using ARMv7-A LDREX/STREX.
 *
 * Simple aligned word load/store is inherently atomic on ARMv7.
 * Read-modify-write operations use inline assembly to guarantee
 * no intervening memory accesses between LDREX and STREX.
 * Memory barriers use __dmb (Data Memory Barrier).
 *
 * Return value semantics:
 *   - inc/dec/cmpxchg/xchg: return OLD value
 *   - add/sub:              return NEW value  (matches __atomic_add_fetch/sub_fetch)
 */

intptr_t aosl_hal_atomic_read(const intptr_t *v)
{
  return *v;
}

void aosl_hal_atomic_set(intptr_t *v, intptr_t i)
{
  *v = i;
}

intptr_t aosl_hal_atomic_inc(intptr_t *v)
{
  intptr_t old, new_val;
  int status;
  do {
    __asm {
      LDREX old, [v]
      ADD   new_val, old, #1
      STREX status, new_val, [v]
    }
  } while (status != 0);
  return old;
}

intptr_t aosl_hal_atomic_dec(intptr_t *v)
{
  intptr_t old, new_val;
  int status;
  do {
    __asm {
      LDREX old, [v]
      SUB   new_val, old, #1
      STREX status, new_val, [v]
    }
  } while (status != 0);
  return old;
}

intptr_t aosl_hal_atomic_add(intptr_t i, intptr_t *v)
{
  intptr_t old, new_val;
  int status;
  do {
    __asm {
      LDREX old, [v]
      ADD   new_val, old, i
      STREX status, new_val, [v]
    }
  } while (status != 0);
  return new_val;
}

intptr_t aosl_hal_atomic_sub(intptr_t i, intptr_t *v)
{
  intptr_t old, new_val;
  int status;
  do {
    __asm {
      LDREX old, [v]
      SUB   new_val, old, i
      STREX status, new_val, [v]
    }
  } while (status != 0);
  return new_val;
}

intptr_t aosl_hal_atomic_cmpxchg(intptr_t *v, intptr_t old, intptr_t new)
{
  intptr_t cur;
  int status;
  do {
    __asm {
      LDREX cur, [v]
    }
    if (cur != old) {
      __clrex();
      return cur;
    }
    __asm {
      STREX status, new, [v]
    }
  } while (status != 0);
  return cur;
}

intptr_t aosl_hal_atomic_xchg(intptr_t *v, intptr_t new)
{
  intptr_t old;
  int status;
  do {
    __asm {
      LDREX old, [v]
      STREX status, new, [v]
    }
  } while (status != 0);
  return old;
}

void aosl_hal_mb(void)
{
  __dmb(0xf);
}

void aosl_hal_rmb(void)
{
  __dmb(0xf);
}

void aosl_hal_wmb(void)
{
  __dmb(0xf);
}