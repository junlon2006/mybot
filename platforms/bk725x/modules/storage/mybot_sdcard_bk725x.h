/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_SDCARD_BK725X_H_
#define MYBOT_SDCARD_BK725X_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Acquire a reference to the FATFS SD card mount at VFS_SD_0_PATITION_0.
 * Mounts the card on the first reference; subsequent calls only increment the
 * reference count. Returns 0 when the card is available, -1 on mount failure. */
int mybot_sdcard_bk725x_acquire(void);

/* Release one reference. When the last reference is released the card is
 * unmounted. Safe to call when no reference is held (no-op). */
void mybot_sdcard_bk725x_release(void);

/* Returns true while at least one reference is held. */
bool mybot_sdcard_bk725x_is_mounted(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_SDCARD_BK725X_H_ */
