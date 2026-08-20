/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_RTC_SESSION_H_
#define MYBOT_RTC_SESSION_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RTC session states */
typedef enum {
    MYBOT_RTC_STATE_IDLE,
    MYBOT_RTC_STATE_INITIALIZED,
    MYBOT_RTC_STATE_CONNECTING,
    MYBOT_RTC_STATE_CONNECTED, /* joined channel successfully */
    MYBOT_RTC_STATE_RECONNECTING,
    MYBOT_RTC_STATE_DISCONNECTED,
    MYBOT_RTC_STATE_ERROR,
} mybot_rtc_state_t;

/* Callbacks from the RTC session to the application. State callbacks may run
 * on an SDK callback thread or on the thread invoking a session API. */
typedef struct {
    /** Called when remote audio PCM data arrives.
     *  @param uid      remote user ID
     *  @param data     PCM buffer (16-bit, 16 kHz, mono)
     *  @param len      buffer length in bytes
     */
    void (*on_remote_audio)(uint32_t uid, const void *data, size_t len);

    /** Called when the SDK asks the application to renew the channel token. */
    void (*on_token_will_expire)(void);

    /** Called when state changes. */
    void (*on_state_changed)(mybot_rtc_state_t state);
} mybot_rtc_session_callbacks_t;

/* ----------------------------------------------------------
 * RTC Session API
 * ---------------------------------------------------------- */

/** Initialize the RTC session (calls agora_rtc_init).
 *  @param app_id  Agora App ID string
 *  @param cbs     callbacks (may be NULL)
 *  @return 0 on success, -1 on error.
 */
int mybot_rtc_session_init(const char *app_id, mybot_rtc_session_callbacks_t *cbs);

/** Join a channel with a string user account.
 *  @param channel      channel name
 *  @param token        token string (NULL or empty for no token)
 *  @param user_account user account string (max 255 bytes)
 *  @return 0 on success, -1 on error.
 */
int mybot_rtc_session_join(const char *channel, const char *token, const char *user_account);

/** Leave the current channel. */
int mybot_rtc_session_leave(void);

/** Finalize the RTC session. Safe to call when it is not initialized. */
void mybot_rtc_session_fini(void);

/** Send PCM audio data to the channel.
 *  @param data  PCM buffer (16-bit, 16 kHz mic). With Cloud AEC enabled, the
 *               payload uses the service-defined [mic, ref] interleaving while
 *               the RTC PCM channel declaration remains mono.
 *  @param len   complete payload length in bytes
 *  @return 0 on success, -1 on error.
 */
int mybot_rtc_session_send_audio(const void *data, size_t len);

/** Apply a renewed token to the active RTC connection.
 *  @param token renewed RTC token
 *  @return 0 on success, -1 on error.
 */
int mybot_rtc_session_renew_token(const char *token);

/** Get current session state. */
mybot_rtc_state_t mybot_rtc_session_get_state(void);

/** Check if the session is connected. */
bool mybot_rtc_session_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_RTC_SESSION_H_ */
