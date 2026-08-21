/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_CONTROLLER_H_
#define MYBOT_CONTROLLER_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the BK725x Mybot application controller in a PSRAM-backed thread. */
int mybot_controller_start(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_CONTROLLER_H_ */
