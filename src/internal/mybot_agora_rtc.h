/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_AGORA_RTC_H_
#define MYBOT_AGORA_RTC_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MYBOT_RTC_STATE_IDLE = 0,
    MYBOT_RTC_STATE_INITIALIZED,
    MYBOT_RTC_STATE_CONNECTING,
    MYBOT_RTC_STATE_CONNECTED,
    MYBOT_RTC_STATE_RECONNECTING,
    MYBOT_RTC_STATE_DISCONNECTED,
    MYBOT_RTC_STATE_ERROR,
    MYBOT_RTC_STATE_INIT_FAILED,
} mybot_rtc_state_t;

/* Agora RTSA is process-wide and supports one active 1-to-1 conversation.
 * The control owner serializes init, join, leave, and fini. Callbacks must not
 * re-enter this API. Only connection-scoped events update call state; RTSA
 * global errors are logged without being forwarded. */
typedef struct {
    void (*on_remote_audio)(uint32_t uid, const void *data, size_t len, void *user_data);
    void (*on_token_will_expire)(void *user_data);
    void (*on_state_changed)(mybot_rtc_state_t state, void *user_data);
    void *user_data;
} mybot_agora_rtc_callbacks_t;

/** Initialize the process-wide RTSA service once for the current mybot run.
 *  Repeated calls are accepted only after a successful initialization with
 *  the same App ID and while no conversation is active. An initialization
 *  failure disables RTC until the process restarts. */
int mybot_agora_rtc_init(const char *app_id, const mybot_agora_rtc_callbacks_t *callbacks);

/** Create a connection and start joining one 1-to-1 channel. */
int mybot_agora_rtc_join(const char *channel, const char *token, const char *user_account);

/** Leave and destroy the active connection while keeping RTSA initialized. */
int mybot_agora_rtc_leave(void);

/** Finalize the process-wide RTSA service. Call after RTC control and media workers stop. */
void mybot_agora_rtc_fini(void);

/** Send one PCM payload on the active connection. */
int mybot_agora_rtc_send_audio(const void *data, size_t len);

/** Queue a renewed token for the active connection. */
int mybot_agora_rtc_renew_token(const char *token);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_AGORA_RTC_H_ */
