/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_AGORA_RTC_H_
#define MYBOT_AGORA_RTC_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

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

/** RTM lifecycle events forwarded from the Agora SDK. */
typedef enum {
    MYBOT_RTM_EVENT_LOGIN = 0,
    MYBOT_RTM_EVENT_KICKOFF = 1,
    MYBOT_RTM_EVENT_EXIT = 2,
} mybot_rtm_event_type_t;

/** RTM delivery state forwarded by the Agora SDK. */
typedef enum {
    MYBOT_RTM_MSG_STATE_INIT = 0,
    MYBOT_RTM_MSG_STATE_RECEIVED,
    MYBOT_RTM_MSG_STATE_UNREACHABLE,
    MYBOT_RTM_MSG_STATE_TIMEOUT,
} mybot_rtm_message_state_t;

/** Maximum byte capacity of an RTM account including its terminating NUL. */
#define MYBOT_RTM_UID_MAX_LEN 64

/* Agora RTSA is process-wide and supports one active 1-to-1 conversation.
 * The control owner serializes init, join, leave, and fini. Callbacks must not
 * re-enter this API. Only connection-scoped events update call state; RTSA
 * global errors are logged without being forwarded. */
typedef struct {
    void (*on_remote_audio)(uint32_t uid, const void *data, size_t len, void *user_data);
    void (*on_token_will_expire)(void *user_data);
    void (*on_state_changed)(mybot_rtc_state_t state, void *user_data);
    void (*on_rtm_event)(const char *rtm_uid, mybot_rtm_event_type_t event_type, int error_code,
                         void *user_data);
    void (*on_rtm_data)(const char *rtm_uid, const void *data, size_t len, const char *custom_type,
                        void *user_data);
    void (*on_rtm_subscribe_result)(const char *channel, int error_code, void *user_data);
    void (*on_rtm_subscribe_data)(const char *channel, const char *rtm_uid, const void *data,
                                  size_t len, const char *custom_type, void *user_data);
    void (*on_rtm_send_data_result)(const char *rtm_uid, uint32_t msg_id,
                                    mybot_rtm_message_state_t state, void *user_data);
    void *user_data;
} mybot_agora_rtc_callbacks_t;

/** Initialize the process-wide RTSA service once for the current mybot run.
 *  Repeated calls are accepted only after a successful initialization with
 *  the same App ID and while no conversation is active. An initialization
 *  failure disables RTC until the process restarts. */
int mybot_agora_rtc_init(const char *app_id, const mybot_agora_rtc_callbacks_t *callbacks);

/**
 * Validate an RTM account according to the Agora RTSA contract.
 *
 * The account must be non-empty, shorter than MYBOT_RTM_UID_MAX_LEN bytes, and
 * contain only ASCII letters, digits, space, or the punctuation characters
 * accepted by agora_rtm_login(). The account is normally the server-issued
 * rtc.uid/local_uid from the conversation response; do not derive it from the
 * device id unless the service explicitly assigns that value.
 */
bool mybot_agora_rtc_rtm_uid_is_valid(const char *rtm_uid);

/** Log in to RTM using the server-issued local RTC UID and RTM token. */
int mybot_agora_rtc_login_rtm(const char *rtm_uid, const char *rtm_token);

/** Log out of RTM if a login has been requested or completed. */
int mybot_agora_rtc_logout_rtm(void);

/** Send one point-to-point RTM message after the login event succeeds. */
int mybot_agora_rtc_send_rtm_data(const char *peer_rtm_uid, const void *data, size_t len,
                                  uint32_t msg_id, const char *custom_type);

/** Return whether the RTM login event has completed successfully. */
bool mybot_agora_rtc_is_rtm_logged_in(void);

/** Create a connection and start joining one 1-to-1 channel. RTM login is
 * requested first using user_account as the local RTM UID and token as the RTM
 * token. After RTM login succeeds, this subscribes to the RTM channel with the
 * same name and waits for subscription success before starting RTC join. */
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
