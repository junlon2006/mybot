/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_BUTTON_H_
#define MYBOT_BUTTON_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the BK725x board GPIO scanner. Button actions are posted to mybot_event. */
int mybot_button_init(void);

/* Stops the GPIO scanner. */
void mybot_button_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_BUTTON_H_ */
