/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_ANNOUNCE_PCM_BK725X_H_
#define MYBOT_ANNOUNCE_PCM_BK725X_H_

#include <mybot/platform/mybot_announce.h>

int mybot_announce_pcm_bk725x_init(void **ctx);
void *mybot_announce_pcm_bk725x_open(void *ctx, mybot_announce_sound_t sound);
int mybot_announce_pcm_bk725x_read(void *ctx, void *sound, int16_t *dst, int max_frames);
void mybot_announce_pcm_bk725x_close(void *ctx, void *sound);
void mybot_announce_pcm_bk725x_destroy(void *ctx);

#endif /* MYBOT_ANNOUNCE_PCM_BK725X_H_ */
