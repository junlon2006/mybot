/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_ANNOUNCE_INTERNAL_H_
#define MYBOT_ANNOUNCE_INTERNAL_H_

#include <mybot/platform/mybot_announce.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SDK-internal announcement facade.
 *
 * The public mybot/platform/mybot_announce.h only exposes the backend contract
 * (ops table + mybot_announce_register()); the SDK core drives the registered
 * backend through the functions below. When a pair code is obtained the core
 * queues the fixed prompt followed by one sound per digit and streams those
 * sounds into the playback ring buffer, so the prompt plays once through the
 * normal speaker path without an RTC session.
 */

/** Whether the current platform registered an announcement backend. */
bool mybot_announce_is_registered(void);

/** Initialize the registered backend. No-op when none is registered. */
int mybot_announce_init(void);

/** Release the backend and any queued announcement. Idempotent. */
void mybot_announce_deinit(void);

/**
 * Start the pairing-code announcement: fixed prompt + one sound per digit.
 *
 * @param code pair code from the device service
 * @return 0 when playback was queued, -1 when the feature is disabled or the
 *         prompt asset is unavailable (nothing is played in that case).
 */
int mybot_announce_play_pair_code(const char *code);

/** Stop the announcement and drop any unplayed audio. Idempotent. */
void mybot_announce_stop(void);

/** Whether an announcement still has PCM left to play. */
bool mybot_announce_is_active(void);

/** Copy the next announcement PCM frames (16 kHz mono s16).
 *  @return frames copied (0 when idle/finished); the caller writes exactly
 *          that many frames into the playback path. */
int mybot_announce_read_pcm(int16_t *dst, int max_frames);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_ANNOUNCE_INTERNAL_H_ */
