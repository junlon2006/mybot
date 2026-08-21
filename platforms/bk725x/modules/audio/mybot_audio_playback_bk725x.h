/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_AUDIO_PLAYBACK_BK725X_H_
#define MYBOT_AUDIO_PLAYBACK_BK725X_H_

#include <stdbool.h>
#include <stdint.h>

#define MYBOT_PLAYBACK_RATE_HZ 16000
#define MYBOT_PLAYBACK_CHANNELS 1
#define MYBOT_PLAYBACK_BITS 16

#ifdef __cplusplus
extern "C" {
#endif

int mybot_audio_bk725x_playback_init(void **ctx, int rate, int channels, int bits);
int mybot_audio_bk725x_playback_start(void *ctx);
int mybot_audio_bk725x_playback_write(void *ctx, const void *buf, int frames);
int mybot_audio_bk725x_playback_stop(void *ctx);
void mybot_audio_bk725x_playback_destroy(void *ctx);
bool mybot_audio_bk725x_playback_is_active(void);
int mybot_audio_bk725x_playback_set_digital_gain(uint8_t gain);
int mybot_audio_bk725x_playback_get_digital_gain(uint8_t *gain);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_AUDIO_PLAYBACK_BK725X_H_ */
