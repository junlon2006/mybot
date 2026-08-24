/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_RTC_SESSION_H_
#define MYBOT_RTC_SESSION_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <api/aosl_atomic.h>
#include <hal/aosl_hal_thread.h>

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
 * on an SDK callback thread or on the thread invoking a session API. The owner
 * must perform session teardown after the callback returns, not re-entrantly
 * from an RTC callback. */
typedef struct {
    /** Called when remote audio PCM data arrives.
     *  @param uid      remote user ID
     *  @param data     PCM buffer (16-bit, 16 kHz, mono)
     *  @param len      buffer length in bytes
     */
    void (*on_remote_audio)(uint32_t uid, const void *data, size_t len, void *user_data);

    /** Called when the SDK asks the application to renew the channel token. */
    void (*on_token_will_expire)(void *user_data);

    /** Called when state changes. */
    void (*on_state_changed)(mybot_rtc_state_t state, void *user_data);

    /** Opaque owner context passed to every callback. */
    void *user_data;
} mybot_rtc_session_callbacks_t;

/** Caller-owned state for one RTC session instance. */
typedef struct {
    aosl_atomic_t state;
    mybot_rtc_session_callbacks_t cbs;
    uint32_t conn_id;
    uint32_t callback_conn_id;
    unsigned int callback_count;
    bool callback_closing;
    aosl_atomic_t initialized;
    aosl_mutex_t lock;
} mybot_rtc_session_t;

/* ----------------------------------------------------------
 * RTC Session API
 * ---------------------------------------------------------- */

/** Initialize the RTC session (calls agora_rtc_init).
 *  @param app_id  Agora App ID string
 *  @param cbs     callbacks (may be NULL)
 *  @return 0 on success, -1 on error.
 */
int mybot_rtc_session_init(mybot_rtc_session_t *session, const char *app_id,
                           const mybot_rtc_session_callbacks_t *cbs);

/** Join a channel with a string user account.
 *  @param channel      channel name
 *  @param token        token string (NULL or empty for no token)
 *  @param user_account user account string (max 255 bytes)
 *  @return 0 on success, -1 on error.
 */
int mybot_rtc_session_join(mybot_rtc_session_t *session, const char *channel, const char *token,
                           const char *user_account);

/** Leave the current channel. */
int mybot_rtc_session_leave(mybot_rtc_session_t *session);

/** Finalize the RTC session. Safe to call when it is not initialized. */
void mybot_rtc_session_fini(mybot_rtc_session_t *session);

/** Send PCM audio data to the channel.
 *  @param data  PCM buffer (16-bit, 16 kHz mic). With Cloud AEC enabled, the
 *               payload uses the service-defined [mic, ref] interleaving while
 *               the RTC PCM channel declaration remains mono.
 *  @param len   complete payload length in bytes
 *  @return 0 on success, -1 on error.
 */
int mybot_rtc_session_send_audio(mybot_rtc_session_t *session, const void *data, size_t len);

/** Apply a renewed token to the active RTC connection.
 *  @param token renewed RTC token
 *  @return 0 on success, -1 on error.
 */
int mybot_rtc_session_renew_token(mybot_rtc_session_t *session, const char *token);

/** Get current session state. */
mybot_rtc_state_t mybot_rtc_session_get_state(const mybot_rtc_session_t *session);

/** Check if the session is connected. */
bool mybot_rtc_session_is_connected(const mybot_rtc_session_t *session);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_RTC_SESSION_H_ */
