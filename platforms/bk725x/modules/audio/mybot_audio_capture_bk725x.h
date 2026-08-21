/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_AUDIO_CAPTURE_BK725X_H_
#define MYBOT_AUDIO_CAPTURE_BK725X_H_

#ifdef __cplusplus
extern "C" {
#endif

int mybot_audio_bk725x_capture_init(void **ctx, int rate, int channels, int bits);
int mybot_audio_bk725x_capture_start(void *ctx);
int mybot_audio_bk725x_capture_read(void *ctx, void *buf, int frames);
int mybot_audio_bk725x_capture_stop(void *ctx);
void mybot_audio_bk725x_capture_destroy(void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_AUDIO_CAPTURE_BK725X_H_ */
