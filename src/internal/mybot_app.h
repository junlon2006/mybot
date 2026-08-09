/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_APP_INTERNAL_H_
#define MYBOT_APP_INTERNAL_H_

/*
 * Internal application-control entry points.
 *
 * These are the request APIs behind user-initiated actions. The SDK core
 * invokes them from platform event handlers (key events, wake words); host
 * applications must not call them directly. Not exported: declarations carry
 * no MYBOT_API, and the library builds with -fvisibility=hidden.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Start a conversation. Called by the core on conversation-start key / wake-word events. */
void mybot_app_start_conversation(void);

/** Stop the current conversation. Called by the core on conversation-stop key events. */
void mybot_app_stop_conversation(void);

/** Request (re-)pairing. Called by the core on pair key events. */
void mybot_app_pair(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_APP_INTERNAL_H_ */
