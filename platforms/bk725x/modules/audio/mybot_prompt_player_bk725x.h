/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_PROMPT_PLAYER_BK725X_H_
#define MYBOT_PROMPT_PLAYER_BK725X_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the local "enter provisioning mode" PCM prompt asynchronously. */
int mybot_prompt_player_bk725x_play_provisioning(void);

/* Starts the local "provisioning success" PCM prompt asynchronously. */
int mybot_prompt_player_bk725x_play_success(void);

/* Cancels any prompt and waits until its playback resources are released. */
void mybot_prompt_player_bk725x_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_PROMPT_PLAYER_BK725X_H_ */
