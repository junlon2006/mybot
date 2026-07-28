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
    RTC_STATE_IDLE,
    RTC_STATE_INITIALIZED,
    RTC_STATE_CONNECTING,
    RTC_STATE_CONNECTED,      /* joined channel successfully */
    RTC_STATE_RECONNECTING,
    RTC_STATE_DISCONNECTED,
    RTC_STATE_ERROR,
} rtc_state_t;

/* Callbacks from RTC session to application (called from SDK thread) */
typedef struct {
    /** Called when remote audio PCM data arrives.
     *  @param uid      remote user ID
     *  @param data     PCM buffer (16-bit, 16 kHz, mono)
     *  @param len      buffer length in bytes
     */
    void (*on_remote_audio)(uint32_t uid, const void *data, size_t len);

    /** Called when state changes. */
    void (*on_state_changed)(rtc_state_t state);
} rtc_session_callbacks_t;

/* ----------------------------------------------------------
 * RTC Session API
 * ---------------------------------------------------------- */

/** Initialize the RTC session (calls agora_rtc_init).
 *  @param app_id  Agora App ID string
 *  @param cbs     callbacks (may be NULL)
 *  @return 0 on success, -1 on error.
 */
int rtc_session_init(const char *app_id, rtc_session_callbacks_t *cbs);

/** Join a channel with a string user account.
 *  @param channel      channel name
 *  @param token        token string (NULL or empty for no token)
 *  @param user_account user account string (max 255 bytes)
 *  @return 0 on success, -1 on error.
 */
int rtc_session_join(const char *channel, const char *token, const char *user_account);

/** Leave the current channel. */
int rtc_session_leave(void);

/** Finalize the RTC session (calls agora_rtc_fini). */
void rtc_session_fini(void);

/** Send PCM audio data to the channel.
 *  @param data  PCM buffer (16-bit, 16 kHz, mono)
 *  @param len   buffer length in bytes
 *  @return 0 on success, -1 on error.
 */
int rtc_session_send_audio(const void *data, size_t len);

/** Get current session state. */
rtc_state_t rtc_session_get_state(void);

/** Check if the session is connected. */
bool rtc_session_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_RTC_SESSION_H_ */
