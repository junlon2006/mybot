/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_AUDIO_INTERNAL_BK725X_H_
#define MYBOT_AUDIO_INTERNAL_BK725X_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Re-apply the currently known volume to whatever playback pipeline is active.
 * A freshly initialized playback pipeline resets its speaker digital gain to the
 * default, so callers that build their own pipeline (e.g. the provisioning PCM
 * prompt player) invoke this right after init to restore the user's volume.
 *
 * The volume may be unknown here: the SDK (and its volume_init) only runs after
 * the WiFi connects, so provisioning reached before any successful connect has
 * never loaded the persisted volume. As a fallback, load it on demand so the
 * prompt still respects the user's last adjustment.
 *
 * Returns 0 when the gain was applied, -1 when no volume is known or persisted
 * and no active playback exists to receive it. */
int mybot_audio_bk725x_volume_apply(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_AUDIO_INTERNAL_BK725X_H_ */
