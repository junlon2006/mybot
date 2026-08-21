/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_AUDIO_VOLUME_BK725X_H_
#define MYBOT_AUDIO_VOLUME_BK725X_H_

#ifdef __cplusplus
extern "C" {
#endif

int mybot_audio_bk725x_volume_init(void **ctx);
int mybot_audio_bk725x_volume_set(void *ctx, int requested_volume);
int mybot_audio_bk725x_volume_get(void *ctx, int *current_volume);
void mybot_audio_bk725x_volume_destroy(void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_AUDIO_VOLUME_BK725X_H_ */
