/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_LINUX_ALSA_PCM_H_
#define MYBOT_LINUX_ALSA_PCM_H_

#include <alsa/asoundlib.h>

#define PCM_POLL_TIMEOUT_MS 50

typedef enum {
    PCM_START_ON_FIRST_FRAME,
    PCM_START_ON_FULL_BUFFER,
} linux_alsa_start_policy_t;

snd_pcm_t *linux_alsa_open_pcm(snd_pcm_stream_t stream, int rate, int channels, int buffer_ms,
                               linux_alsa_start_policy_t start);
int linux_alsa_recover_xrun(snd_pcm_t *handle, const char *kind);
int linux_alsa_recover_suspend(snd_pcm_t *handle);

#endif /* MYBOT_LINUX_ALSA_PCM_H_ */
