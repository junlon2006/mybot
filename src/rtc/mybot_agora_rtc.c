/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_agora_rtc.h"

#include <mybot/mybot_build_config.h>

#include "agora_rtc_api.h"
#include <api/aosl_atomic.h>
#include <api/aosl_log.h>
#include <api/aosl_time.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifndef MYBOT_RTM_LOGIN_TIMEOUT_MS
#define MYBOT_RTM_LOGIN_TIMEOUT_MS 5000U
#endif

#ifndef MYBOT_RTM_SUBSCRIBE_TIMEOUT_MS
#define MYBOT_RTM_SUBSCRIBE_TIMEOUT_MS 5000U
#endif

#define MYBOT_RTM_LOGIN_POLL_MS 10U

typedef struct {
    mybot_rtc_state_t state;
    connection_id_t conn_id;
    mybot_agora_rtc_callbacks_t callbacks;
    char app_id[64];
    char rtm_uid[MYBOT_RTM_UID_MAX_LEN];
    char rtm_channel[AGORA_RTC_CHANNEL_NAME_MAX_LEN + 1];
    bool rtm_login_requested;
    bool rtm_login_completed;
    bool rtm_logged_in;
    bool rtm_subscribe_requested;
    bool rtm_subscribe_completed;
    bool rtm_subscribed;
    bool fini_failed;
} mybot_agora_rtc_t;

static mybot_agora_rtc_t s_rtc = {.conn_id = CONNECTION_ID_INVALID};
static aosl_atomic_t s_rtc_lifecycle_gate;

static void clear_rtm_subscription_locked(void);
static void clear_rtm_login_locked(void);

static void rtc_lock(void) {
    while (aosl_atomic_cmpxchg(&s_rtc_lifecycle_gate, 0, 1) != 0) {
        aosl_msleep(5);
    }
}

static void rtc_unlock(void) {
    aosl_atomic_set(&s_rtc_lifecycle_gate, 0);
}

static const char *state_name(mybot_rtc_state_t state) {
    switch (state) {
    case MYBOT_RTC_STATE_IDLE:
        return "IDLE";
    case MYBOT_RTC_STATE_INITIALIZED:
        return "INITIALIZED";
    case MYBOT_RTC_STATE_CONNECTING:
        return "CONNECTING";
    case MYBOT_RTC_STATE_CONNECTED:
        return "CONNECTED";
    case MYBOT_RTC_STATE_RECONNECTING:
        return "RECONNECTING";
    case MYBOT_RTC_STATE_DISCONNECTED:
        return "DISCONNECTED";
    case MYBOT_RTC_STATE_ERROR:
        return "ERROR";
    case MYBOT_RTC_STATE_INIT_FAILED:
        return "INIT_FAILED";
    }
    return "?";
}

/* RTC callbacks are serialized with media send and connection teardown. */
static void set_state(mybot_rtc_state_t state) {
    if (s_rtc.state == state) {
        return;
    }
    s_rtc.state = state;
    AOSL_LOG_NTC("[RTC] state -> %s", state_name(state));
    if (s_rtc.callbacks.on_state_changed) {
        s_rtc.callbacks.on_state_changed(state, s_rtc.callbacks.user_data);
    }
}

static inline bool connection_is_active(connection_id_t conn_id) {
    return conn_id != CONNECTION_ID_INVALID && conn_id == s_rtc.conn_id;
}

static bool rtm_uid_char_is_allowed(unsigned char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    switch (c) {
    case ' ':
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '(':
    case ')':
    case '+':
    case ',':
    case '-':
    case '.':
    case ':':
    case ';':
    case '<':
    case '=':
    case '>':
    case '?':
    case '@':
    case '[':
    case ']':
    case '^':
    case '_':
    case '{':
    case '|':
    case '}':
    case '~':
        return true;
    default:
        return false;
    }
}

bool mybot_agora_rtc_rtm_uid_is_valid(const char *rtm_uid) {
    if (!rtm_uid || !rtm_uid[0]) {
        return false;
    }
    size_t len = strlen(rtm_uid);
    if (len >= MYBOT_RTM_UID_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (!rtm_uid_char_is_allowed((unsigned char)rtm_uid[i])) {
            return false;
        }
    }
    return true;
}

static bool map_rtm_event(rtm_event_type_e event_type, mybot_rtm_event_type_t *mapped) {
    if (!mapped) {
        return false;
    }
    switch (event_type) {
    case RTM_EVENT_TYPE_LOGIN:
        *mapped = MYBOT_RTM_EVENT_LOGIN;
        return true;
    case RTM_EVENT_TYPE_KICKOFF:
        *mapped = MYBOT_RTM_EVENT_KICKOFF;
        return true;
    case RTM_EVENT_TYPE_EXIT:
        *mapped = MYBOT_RTM_EVENT_EXIT;
        return true;
    default:
        return false;
    }
}

static const char *rtm_event_name(rtm_event_type_e event_type) {
    switch (event_type) {
    case RTM_EVENT_TYPE_LOGIN:
        return "LOGIN";
    case RTM_EVENT_TYPE_KICKOFF:
        return "KICKOFF";
    case RTM_EVENT_TYPE_EXIT:
        return "EXIT";
    default:
        return "UNKNOWN";
    }
}

static const char *rtm_message_type_name(rtm_message_type_e message_type) {
    switch (message_type) {
    case RTM_MESSAGE_TYPE_BINARY:
        return "BINARY";
    case RTM_MESSAGE_TYPE_STRING:
        return "STRING";
    default:
        return "UNKNOWN";
    }
}

static void on_rtm_event(const char *rtm_uid, rtm_event_type_e event_type,
                         rtm_err_code_e error_code) {
    mybot_rtm_event_type_t mapped_event;

    rtc_lock();
    if (!s_rtc.rtm_login_requested) {
        AOSL_LOG_NTC("[RTM] event ignored: login was not requested (type=%s(%d), error=%d)",
                     rtm_event_name(event_type), (int)event_type, (int)error_code);
        rtc_unlock();
        return;
    }
    if (!rtm_uid) {
        AOSL_LOG_WRN("[RTM] event ignored: missing UID (type=%s(%d), error=%d)",
                     rtm_event_name(event_type), (int)event_type, (int)error_code);
        rtc_unlock();
        return;
    }
    if (strcmp(rtm_uid, s_rtc.rtm_uid) != 0) {
        AOSL_LOG_WRN("[RTM] event ignored: UID mismatch (expected=%s, received=%s, type=%s(%d))",
                     s_rtc.rtm_uid, rtm_uid, rtm_event_name(event_type), (int)event_type);
        rtc_unlock();
        return;
    }

    AOSL_LOG_NTC("[RTM] event: uid=%s type=%s(%d) error=%d", rtm_uid, rtm_event_name(event_type),
                 (int)event_type, (int)error_code);

    switch (event_type) {
    case RTM_EVENT_TYPE_LOGIN:
        s_rtc.rtm_login_completed = true;
        s_rtc.rtm_logged_in = error_code == ERR_RTM_OK;
        if (s_rtc.rtm_logged_in) {
            AOSL_LOG_NTC("[RTM] login succeeded (uid=%s)", rtm_uid);
        } else {
            clear_rtm_login_locked();
            AOSL_LOG_WRN("[RTM] login failed (uid=%s, error=%d)", rtm_uid, (int)error_code);
        }
        break;
    case RTM_EVENT_TYPE_KICKOFF:
        clear_rtm_login_locked();
        AOSL_LOG_WRN("[RTM] account kicked off (uid=%s, error=%d)", rtm_uid, (int)error_code);
        break;
    case RTM_EVENT_TYPE_EXIT:
        clear_rtm_login_locked();
        AOSL_LOG_NTC("[RTM] account exited (uid=%s, error=%d)", rtm_uid, (int)error_code);
        break;
    default:
        AOSL_LOG_WRN("[RTM] unknown event received (uid=%s, type=%d, error=%d)", rtm_uid,
                     (int)event_type, (int)error_code);
        break;
    }

    if (!map_rtm_event(event_type, &mapped_event)) {
        AOSL_LOG_NTC("[RTM] event not forwarded: unsupported type=%d", (int)event_type);
    } else if (!s_rtc.callbacks.on_rtm_event) {
        AOSL_LOG_NTC("[RTM] event not forwarded: no application callback (type=%s)",
                     rtm_event_name(event_type));
    } else {
        AOSL_LOG_NTC("[RTM] forwarding event to application (type=%s)", rtm_event_name(event_type));
        s_rtc.callbacks.on_rtm_event(rtm_uid, mapped_event, (int)error_code,
                                     s_rtc.callbacks.user_data);
    }
    rtc_unlock();
}

static void on_rtm_data(const char *rtm_uid, const void *data, size_t len,
                        rtm_message_type_e message_type, const char *custom_type) {
    size_t preview_len = len > 512U ? 512U : len;

    rtc_lock();
    if (!s_rtc.rtm_login_requested) {
        AOSL_LOG_NTC(
            "[RTM] data ignored: login was not requested (from=%s, message_type=%s, type=%s, "
            "len=%zu)",
            rtm_uid ? rtm_uid : "(null)", rtm_message_type_name(message_type),
            custom_type ? custom_type : "(null)", len);
        rtc_unlock();
        return;
    }

    if (data && preview_len > 0) {
        AOSL_LOG_NTC("[RTM] data received: from=%s message_type=%s type=%s len=%zu msg=%.*s",
                     rtm_uid ? rtm_uid : "(null)", rtm_message_type_name(message_type),
                     custom_type ? custom_type : "(null)", len, (int)preview_len,
                     (const char *)data);
    } else {
        AOSL_LOG_NTC("[RTM] data received: from=%s message_type=%s type=%s len=%zu msg=(empty)",
                     rtm_uid ? rtm_uid : "(null)", rtm_message_type_name(message_type),
                     custom_type ? custom_type : "(null)", len);
    }

    if (s_rtc.callbacks.on_rtm_data) {
        AOSL_LOG_NTC("[RTM] forwarding data to application (from=%s, len=%zu)",
                     rtm_uid ? rtm_uid : "(null)", len);
        s_rtc.callbacks.on_rtm_data(rtm_uid, data, len, custom_type, s_rtc.callbacks.user_data);
    } else {
        AOSL_LOG_NTC("[RTM] data not forwarded: no application callback (from=%s, len=%zu)",
                     rtm_uid ? rtm_uid : "(null)", len);
    }
    rtc_unlock();
}

static void on_rtm_subscribe_result(const char *channel, rtm_err_code_e error_code) {
    rtc_lock();
    if (!s_rtc.rtm_subscribe_requested) {
        AOSL_LOG_NTC("[RTM] subscribe result ignored: no subscription requested (channel=%s, "
                     "error=%d)",
                     channel ? channel : "(null)", (int)error_code);
        rtc_unlock();
        return;
    }
    if (!channel || strcmp(channel, s_rtc.rtm_channel) != 0) {
        AOSL_LOG_WRN("[RTM] subscribe result ignored: channel mismatch (expected=%s, received=%s)",
                     s_rtc.rtm_channel, channel ? channel : "(null)");
        rtc_unlock();
        return;
    }

    s_rtc.rtm_subscribe_completed = true;
    s_rtc.rtm_subscribed = error_code == ERR_RTM_OK;
    if (s_rtc.rtm_subscribed) {
        AOSL_LOG_NTC("[RTM] channel subscription succeeded (channel=%s)", channel);
    } else {
        AOSL_LOG_WRN("[RTM] channel subscription failed (channel=%s, error=%d)", channel,
                     (int)error_code);
    }

    if (s_rtc.callbacks.on_rtm_subscribe_result) {
        AOSL_LOG_NTC("[RTM] forwarding subscribe result to application (channel=%s)", channel);
        s_rtc.callbacks.on_rtm_subscribe_result(channel, (int)error_code,
                                                s_rtc.callbacks.user_data);
    } else {
        AOSL_LOG_NTC("[RTM] subscribe result not forwarded: no application callback (channel=%s)",
                     channel);
    }
    rtc_unlock();
}

static void on_rtm_subscribe_data(const char *channel, const char *rtm_uid, const void *data,
                                  size_t len, rtm_message_type_e message_type,
                                  const char *custom_type) {
    size_t preview_len = len > 512U ? 512U : len;

    rtc_lock();
    if (!s_rtc.rtm_subscribed) {
        AOSL_LOG_NTC("[RTM] channel data ignored: no active subscription (channel=%s, from=%s, "
                     "len=%zu)",
                     channel ? channel : "(null)", rtm_uid ? rtm_uid : "(null)", len);
        rtc_unlock();
        return;
    }
    if (!channel || strcmp(channel, s_rtc.rtm_channel) != 0) {
        AOSL_LOG_WRN("[RTM] channel data ignored: channel mismatch (expected=%s, received=%s)",
                     s_rtc.rtm_channel, channel ? channel : "(null)");
        rtc_unlock();
        return;
    }

    if (data && preview_len > 0) {
        AOSL_LOG_NTC("[RTM] channel data received: channel=%s from=%s message_type=%s type=%s "
                     "len=%zu msg=%.*s",
                     channel, rtm_uid ? rtm_uid : "(null)", rtm_message_type_name(message_type),
                     custom_type ? custom_type : "(null)", len, (int)preview_len,
                     (const char *)data);
    } else {
        AOSL_LOG_NTC("[RTM] channel data received: channel=%s from=%s message_type=%s type=%s "
                     "len=%zu msg=(empty)",
                     channel, rtm_uid ? rtm_uid : "(null)", rtm_message_type_name(message_type),
                     custom_type ? custom_type : "(null)", len);
    }

    if (s_rtc.callbacks.on_rtm_subscribe_data) {
        AOSL_LOG_NTC("[RTM] forwarding channel data to application (channel=%s, from=%s, len=%zu)",
                     channel, rtm_uid ? rtm_uid : "(null)", len);
        s_rtc.callbacks.on_rtm_subscribe_data(channel, rtm_uid, data, len, custom_type,
                                              s_rtc.callbacks.user_data);
    } else {
        AOSL_LOG_NTC("[RTM] channel data not forwarded: no application callback (channel=%s, "
                     "from=%s, len=%zu)",
                     channel, rtm_uid ? rtm_uid : "(null)", len);
    }
    rtc_unlock();
}

static void on_rtm_send_data_result(const char *rtm_uid, uint32_t msg_id, rtm_msg_state_e state) {
    rtc_lock();
    if (s_rtc.rtm_login_requested && s_rtc.callbacks.on_rtm_send_data_result) {
        s_rtc.callbacks.on_rtm_send_data_result(rtm_uid, msg_id, (mybot_rtm_message_state_t)state,
                                                s_rtc.callbacks.user_data);
    }
    rtc_unlock();
}

static int login_rtm_locked(const char *rtm_uid, const char *rtm_token) {
    if (!mybot_agora_rtc_rtm_uid_is_valid(rtm_uid)) {
        AOSL_LOG_ERR("[RTM] login rejected: invalid RTM UID");
        return -1;
    }
    if (s_rtc.state != MYBOT_RTC_STATE_INITIALIZED || s_rtc.conn_id != CONNECTION_ID_INVALID) {
        AOSL_LOG_ERR("[RTM] login rejected (RTC state=%s, conn_id=%u)", state_name(s_rtc.state),
                     s_rtc.conn_id);
        return -1;
    }
    if (s_rtc.rtm_login_requested) {
        return strcmp(s_rtc.rtm_uid, rtm_uid) == 0 ? 0 : -1;
    }

    agora_rtm_handler_t handler;
    memset(&handler, 0, sizeof(handler));
    handler.on_rtm_event = on_rtm_event;
    handler.on_rtm_data = on_rtm_data;
    handler.on_rtm_send_data_result = on_rtm_send_data_result;
    handler.on_rtm_subscribe_result = on_rtm_subscribe_result;
    handler.on_rtm_subscribe_data = on_rtm_subscribe_data;

    memcpy(s_rtc.rtm_uid, rtm_uid, strlen(rtm_uid) + 1);
    s_rtc.rtm_login_requested = true;
    s_rtc.rtm_login_completed = false;
    s_rtc.rtm_logged_in = false;
    int ret = agora_rtm_login(rtm_uid, rtm_token && rtm_token[0] ? rtm_token : NULL, &handler);
    if (ret < 0) {
        AOSL_LOG_ERR("[RTM] login failed: %s", agora_rtc_err_2_str(ret));
        clear_rtm_login_locked();
        return -1;
    }
    AOSL_LOG_NTC("[RTM] login requested (uid=%s)", rtm_uid);
    return 0;
}

static int wait_for_rtm_login(const char *rtm_uid) {
    aosl_ts_t started_at = aosl_tick_ms();
    AOSL_LOG_NTC("[RTM] waiting for login before RTC join (uid=%s, timeout=%u ms)", rtm_uid,
                 (unsigned int)MYBOT_RTM_LOGIN_TIMEOUT_MS);

    for (;;) {
        rtc_lock();
        bool request_matches = s_rtc.rtm_login_requested && strcmp(s_rtc.rtm_uid, rtm_uid) == 0;
        bool completed = s_rtc.rtm_login_completed;
        bool logged_in = s_rtc.rtm_logged_in;
        rtc_unlock();

        if (!request_matches) {
            AOSL_LOG_ERR("[RTM] login wait aborted: request is no longer active (uid=%s)", rtm_uid);
            return -1;
        }
        if (completed) {
            if (!logged_in) {
                AOSL_LOG_ERR("[RTM] login did not succeed (uid=%s)", rtm_uid);
                return -1;
            }
            AOSL_LOG_NTC("[RTM] login confirmed before RTC join (uid=%s)", rtm_uid);
            return 0;
        }
        if (aosl_tick_ms() - started_at >= MYBOT_RTM_LOGIN_TIMEOUT_MS) {
            AOSL_LOG_ERR("[RTM] login timed out after %u ms (uid=%s)",
                         (unsigned int)MYBOT_RTM_LOGIN_TIMEOUT_MS, rtm_uid);
            return -1;
        }
        aosl_msleep(MYBOT_RTM_LOGIN_POLL_MS);
    }
}

static void clear_rtm_subscription_locked(void) {
    s_rtc.rtm_channel[0] = '\0';
    s_rtc.rtm_subscribe_requested = false;
    s_rtc.rtm_subscribe_completed = false;
    s_rtc.rtm_subscribed = false;
}

static void clear_rtm_login_locked(void) {
    clear_rtm_subscription_locked();
    s_rtc.rtm_uid[0] = '\0';
    s_rtc.rtm_login_requested = false;
    s_rtc.rtm_login_completed = false;
    s_rtc.rtm_logged_in = false;
}

static int unsubscribe_rtm_locked(void) {
    if (!s_rtc.rtm_subscribe_requested) {
        clear_rtm_subscription_locked();
        return 0;
    }

    int ret = 0;
    if (!s_rtc.rtm_subscribe_completed || s_rtc.rtm_subscribed) {
        ret = agora_rtm_unsubscribe(s_rtc.rtm_channel);
        if (ret < 0) {
            AOSL_LOG_ERR("[RTM] unsubscribe failed (channel=%s): %s", s_rtc.rtm_channel,
                         agora_rtc_err_2_str(ret));
            return ret;
        } else {
            AOSL_LOG_NTC("[RTM] unsubscribed from channel=%s", s_rtc.rtm_channel);
        }
    }
    clear_rtm_subscription_locked();
    return ret;
}

static int subscribe_rtm_locked(const char *channel) {
    if (!channel || !channel[0] || strlen(channel) >= AGORA_RTC_CHANNEL_NAME_MAX_LEN) {
        AOSL_LOG_ERR("[RTM] subscribe rejected: invalid channel");
        return -1;
    }
    if (!s_rtc.rtm_logged_in) {
        AOSL_LOG_ERR("[RTM] subscribe rejected: login has not completed");
        return -1;
    }
    if (s_rtc.rtm_subscribe_requested) {
        return strcmp(s_rtc.rtm_channel, channel) == 0 ? 0 : -1;
    }

    memcpy(s_rtc.rtm_channel, channel, strlen(channel) + 1U);
    s_rtc.rtm_subscribe_requested = true;
    s_rtc.rtm_subscribe_completed = false;
    s_rtc.rtm_subscribed = false;
    int ret = agora_rtm_subscribe(channel);
    if (ret < 0) {
        AOSL_LOG_ERR("[RTM] subscribe failed: %s", agora_rtc_err_2_str(ret));
        clear_rtm_subscription_locked();
        return -1;
    }
    AOSL_LOG_NTC("[RTM] channel subscription requested (channel=%s)", channel);
    return 0;
}

static int wait_for_rtm_subscription(const char *channel) {
    aosl_ts_t started_at = aosl_tick_ms();
    AOSL_LOG_NTC("[RTM] waiting for channel subscription before RTC join (channel=%s, timeout=%u "
                 "ms)",
                 channel, (unsigned int)MYBOT_RTM_SUBSCRIBE_TIMEOUT_MS);

    for (;;) {
        rtc_lock();
        bool request_matches =
            s_rtc.rtm_subscribe_requested && strcmp(s_rtc.rtm_channel, channel) == 0;
        bool completed = s_rtc.rtm_subscribe_completed;
        bool subscribed = s_rtc.rtm_subscribed;
        rtc_unlock();

        if (!request_matches) {
            AOSL_LOG_ERR("[RTM] subscription wait aborted: request is no longer active "
                         "(channel=%s)",
                         channel);
            return -1;
        }
        if (completed) {
            if (!subscribed) {
                AOSL_LOG_ERR("[RTM] channel subscription did not succeed (channel=%s)", channel);
                return -1;
            }
            AOSL_LOG_NTC("[RTM] channel subscription confirmed before RTC join (channel=%s)",
                         channel);
            return 0;
        }
        if (aosl_tick_ms() - started_at >= MYBOT_RTM_SUBSCRIBE_TIMEOUT_MS) {
            AOSL_LOG_ERR("[RTM] channel subscription timed out after %u ms (channel=%s)",
                         (unsigned int)MYBOT_RTM_SUBSCRIBE_TIMEOUT_MS, channel);
            return -1;
        }
        aosl_msleep(MYBOT_RTM_LOGIN_POLL_MS);
    }
}

static void on_join_channel_success(connection_id_t conn_id, uint32_t uid, int elapsed) {
    rtc_lock();
    if (!connection_is_active(conn_id)) {
        rtc_unlock();
        return;
    }
    AOSL_LOG_NTC("join channel success (uid=%u, elapsed=%d ms)", uid, elapsed);
    set_state(MYBOT_RTC_STATE_CONNECTED);
    rtc_unlock();
}

static void on_reconnecting(connection_id_t conn_id) {
    rtc_lock();
    if (!connection_is_active(conn_id)) {
        rtc_unlock();
        return;
    }
    set_state(MYBOT_RTC_STATE_RECONNECTING);
    rtc_unlock();
}

static void on_connection_lost(connection_id_t conn_id) {
    rtc_lock();
    if (!connection_is_active(conn_id)) {
        rtc_unlock();
        return;
    }
    set_state(MYBOT_RTC_STATE_DISCONNECTED);
    rtc_unlock();
}

static void on_rejoin_channel_success(connection_id_t conn_id, uint32_t uid, int elapsed_ms) {
    (void)uid;
    (void)elapsed_ms;
    rtc_lock();
    if (!connection_is_active(conn_id)) {
        rtc_unlock();
        return;
    }
    set_state(MYBOT_RTC_STATE_CONNECTED);
    rtc_unlock();
}

static void on_user_joined(connection_id_t conn_id, const user_info_t *user, int elapsed_ms) {
    (void)elapsed_ms;
    if (!user) {
        return;
    }
    rtc_lock();
    if (!connection_is_active(conn_id)) {
        rtc_unlock();
        return;
    }
    AOSL_LOG_NTC("[RTC] user \"%s\" (uid=%u) joined", user->user_account, user->uid);
    rtc_unlock();
}

static void on_user_offline(connection_id_t conn_id, const user_info_t *user, int reason) {
    if (!user) {
        return;
    }
    rtc_lock();
    if (!connection_is_active(conn_id)) {
        rtc_unlock();
        return;
    }
    AOSL_LOG_NTC("[RTC] user \"%s\" (uid=%u) offline (reason=%d)", user->user_account, user->uid,
                 reason);
    rtc_unlock();
}

static void on_audio_data(connection_id_t conn_id, uint32_t uid, uint16_t sent_ts, const void *data,
                          size_t len, const audio_frame_info_t *info) {
    (void)sent_ts;
    (void)info;
    rtc_lock();
    if (!connection_is_active(conn_id)) {
        rtc_unlock();
        return;
    }
    if (s_rtc.callbacks.on_remote_audio) {
        s_rtc.callbacks.on_remote_audio(uid, data, len, s_rtc.callbacks.user_data);
    }
    rtc_unlock();
}

static void on_error(connection_id_t conn_id, int code, const char *message) {
    if (conn_id == CONNECTION_ID_ALL) {
        AOSL_LOG_ERR("[RTC] global error without connection ownership (code=%d): %s", code,
                     message ? message : "null");
        return;
    }
    rtc_lock();
    if (!connection_is_active(conn_id)) {
        rtc_unlock();
        return;
    }
    AOSL_LOG_ERR("[RTC] error (code=%d): %s", code, message ? message : "null");
    set_state(MYBOT_RTC_STATE_ERROR);
    rtc_unlock();
}

static void on_license_failed(connection_id_t conn_id, int reason) {
    rtc_lock();
    if (!connection_is_active(conn_id)) {
        rtc_unlock();
        return;
    }
    AOSL_LOG_ERR("[RTC] license validation failed (reason=%d)", reason);
    set_state(MYBOT_RTC_STATE_ERROR);
    rtc_unlock();
}

static void on_token_will_expire(connection_id_t conn_id, const char *token) {
    (void)token;
    rtc_lock();
    if (!connection_is_active(conn_id)) {
        rtc_unlock();
        return;
    }
    AOSL_LOG_NTC("[RTC] token privilege will expire");
    if (s_rtc.callbacks.on_token_will_expire) {
        s_rtc.callbacks.on_token_will_expire(s_rtc.callbacks.user_data);
    }
    rtc_unlock();
}

static void clear_runtime_state(void) {
    s_rtc.conn_id = CONNECTION_ID_INVALID;
    memset(&s_rtc.callbacks, 0, sizeof(s_rtc.callbacks));
    s_rtc.app_id[0] = '\0';
    clear_rtm_login_locked();
    s_rtc.fini_failed = false;
}

int mybot_agora_rtc_init(const char *app_id, const mybot_agora_rtc_callbacks_t *callbacks) {
    if (!app_id || !app_id[0] || strlen(app_id) >= sizeof(s_rtc.app_id)) {
        AOSL_LOG_ERR("[RTC] init rejected: invalid App ID");
        return -1;
    }

    rtc_lock();
    if (s_rtc.state == MYBOT_RTC_STATE_INIT_FAILED) {
        AOSL_LOG_ERR("[RTC] init rejected after a previous initialization failure");
        rtc_unlock();
        return -1;
    }

    if (s_rtc.state != MYBOT_RTC_STATE_IDLE) {
        mybot_rtc_state_t state = s_rtc.state;
        bool app_id_matches = strcmp(s_rtc.app_id, app_id) == 0;
        bool ready = state == MYBOT_RTC_STATE_INITIALIZED &&
                     s_rtc.conn_id == CONNECTION_ID_INVALID && app_id_matches;
        if (ready) {
            if (callbacks) {
                s_rtc.callbacks = *callbacks;
            } else {
                memset(&s_rtc.callbacks, 0, sizeof(s_rtc.callbacks));
            }
        }
        rtc_unlock();
        if (!ready) {
            AOSL_LOG_ERR("[RTC] init rejected (state=%s, app_id_match=%d)", state_name(state),
                         app_id_matches ? 1 : 0);
        }
        return ready ? 0 : -1;
    }

    clear_runtime_state();
    if (callbacks) {
        s_rtc.callbacks = *callbacks;
    }
    memcpy(s_rtc.app_id, app_id, strlen(app_id) + 1);

    agora_rtc_event_handler_t handler;
    memset(&handler, 0, sizeof(handler));
    handler.on_join_channel_success = on_join_channel_success;
    handler.on_reconnecting = on_reconnecting;
    handler.on_connection_lost = on_connection_lost;
    handler.on_rejoin_channel_success = on_rejoin_channel_success;
    handler.on_user_joined_with_user_account = on_user_joined;
    handler.on_user_offline_with_user_account = on_user_offline;
    handler.on_audio_data = on_audio_data;
    handler.on_error = on_error;
    handler.on_license_validation_failure = on_license_failed;
    handler.on_token_privilege_will_expire = on_token_will_expire;

    rtc_service_option_t options;
    memset(&options, 0, sizeof(options));
    options.area_code = AREA_CODE_GLOB;
    options.log_cfg.log_level = RTC_LOG_NOTICE;
    options.use_string_uid = true;

    int ret = agora_rtc_init((void *)app_id, &handler, &options);
    if (ret < 0) {
        AOSL_LOG_ERR("agora_rtc_init failed: %s", agora_rtc_err_2_str(ret));
        clear_runtime_state();
        s_rtc.state = MYBOT_RTC_STATE_INIT_FAILED;
        rtc_unlock();
        return -1;
    }

    s_rtc.state = MYBOT_RTC_STATE_INITIALIZED;
    AOSL_LOG_NTC("agora_rtc_init ok (sdk v%s)", agora_rtc_get_version());
    rtc_unlock();
    return 0;
}

int mybot_agora_rtc_login_rtm(const char *rtm_uid, const char *rtm_token) {
    rtc_lock();
    int ret = login_rtm_locked(rtm_uid, rtm_token);
    rtc_unlock();
    return ret;
}

int mybot_agora_rtc_logout_rtm(void) {
    rtc_lock();
    if (!s_rtc.rtm_login_requested) {
        clear_rtm_subscription_locked();
        rtc_unlock();
        return 0;
    }

    int unsubscribe_ret = unsubscribe_rtm_locked();
    int ret = agora_rtm_logout();
    if (ret < 0) {
        AOSL_LOG_ERR("[RTM] logout failed: %s", agora_rtc_err_2_str(ret));
        rtc_unlock();
        return -1;
    }
    AOSL_LOG_NTC("[RTM] logged out (uid=%s)", s_rtc.rtm_uid);
    clear_rtm_login_locked();
    rtc_unlock();
    return unsubscribe_ret < 0 ? -1 : 0;
}

bool mybot_agora_rtc_is_rtm_logged_in(void) {
    rtc_lock();
    bool logged_in = s_rtc.rtm_logged_in;
    rtc_unlock();
    return logged_in;
}

int mybot_agora_rtc_send_rtm_data(const char *peer_rtm_uid, const void *data, size_t len,
                                  uint32_t msg_id, const char *custom_type) {
    if (!mybot_agora_rtc_rtm_uid_is_valid(peer_rtm_uid) || !data || len == 0 || len > 31U * 1024U ||
        (custom_type && strlen(custom_type) > 32U)) {
        AOSL_LOG_ERR("[RTM] send rejected: invalid peer, payload, or custom type");
        return -1;
    }

    rtc_lock();
    if (!s_rtc.rtm_logged_in) {
        AOSL_LOG_ERR("[RTM] send rejected: login has not completed");
        rtc_unlock();
        return -1;
    }
    int ret =
        agora_rtm_send_data(peer_rtm_uid, data, len, msg_id, RTM_MESSAGE_TYPE_BINARY, custom_type);
    if (ret < 0) {
        AOSL_LOG_ERR("[RTM] send failed: %s", agora_rtc_err_2_str(ret));
    }
    rtc_unlock();
    return ret;
}

int mybot_agora_rtc_join(const char *channel, const char *token, const char *user_account) {
    if (!channel || !channel[0] || strlen(channel) >= AGORA_RTC_CHANNEL_NAME_MAX_LEN ||
        !user_account || !user_account[0]) {
        AOSL_LOG_ERR("[RTC] join rejected: invalid channel or user account");
        return -1;
    }
    bool rtm_started_for_join = false;
    rtc_lock();
    if (s_rtc.rtm_login_requested) {
        if (strcmp(s_rtc.rtm_uid, user_account) != 0) {
            AOSL_LOG_ERR("[RTC] join rejected: RTM UID does not match RTC user account");
            rtc_unlock();
            return -1;
        }
    } else {
        if (login_rtm_locked(user_account, token) < 0) {
            rtc_unlock();
            return -1;
        }
        rtm_started_for_join = true;
    }
    rtc_unlock();

    if (wait_for_rtm_login(user_account) < 0) {
        if (rtm_started_for_join) {
            (void)mybot_agora_rtc_logout_rtm();
        }
        return -1;
    }

    bool rtm_subscription_started_for_join = false;
    rtc_lock();
    int ret = -1;
    if (s_rtc.state != MYBOT_RTC_STATE_INITIALIZED || s_rtc.conn_id != CONNECTION_ID_INVALID ||
        !s_rtc.rtm_logged_in || strcmp(s_rtc.rtm_uid, user_account) != 0) {
        AOSL_LOG_ERR("[RTC] join rejected after RTM wait (state=%s, conn_id=%u, rtm_logged_in=%d)",
                     state_name(s_rtc.state), s_rtc.conn_id, s_rtc.rtm_logged_in ? 1 : 0);
        goto out;
    }
    rtm_subscription_started_for_join = !s_rtc.rtm_subscribe_requested;
    if (subscribe_rtm_locked(channel) < 0) {
        goto out;
    }
    rtc_unlock();

    if (wait_for_rtm_subscription(channel) < 0) {
        (void)mybot_agora_rtc_logout_rtm();
        return -1;
    }

    rtc_lock();
    if (s_rtc.state != MYBOT_RTC_STATE_INITIALIZED || s_rtc.conn_id != CONNECTION_ID_INVALID ||
        !s_rtc.rtm_logged_in || !s_rtc.rtm_subscribed || strcmp(s_rtc.rtm_uid, user_account) != 0 ||
        strcmp(s_rtc.rtm_channel, channel) != 0) {
        AOSL_LOG_ERR("[RTC] join rejected after RTM subscription (state=%s, conn_id=%u, "
                     "rtm_logged_in=%d, rtm_subscribed=%d)",
                     state_name(s_rtc.state), s_rtc.conn_id, s_rtc.rtm_logged_in ? 1 : 0,
                     s_rtc.rtm_subscribed ? 1 : 0);
        goto out;
    }

    connection_id_t conn_id = CONNECTION_ID_INVALID;
    ret = agora_rtc_create_connection(&conn_id);
    if (ret < 0) {
        AOSL_LOG_ERR("create_connection failed: %s", agora_rtc_err_2_str(ret));
        goto out;
    }
    AOSL_LOG_NTC("[RTC] connection created (conn_id=%u)", conn_id);

    rtc_channel_options_t channel_options;
    memset(&channel_options, 0, sizeof(channel_options));
    channel_options.auto_subscribe_audio = true;
    channel_options.enable_audio_jitter_buffer = true;
    channel_options.enable_audio_decode = true;
#if MYBOT_CLOUD_AEC
    channel_options.enable_audio_downlink_aec = true;
#endif
#if MYBOT_AI_QOS
    channel_options.enable_audio_ai_qos = true;
#endif
    channel_options.audio_codec_opt.audio_codec_type = AUDIO_CODEC_TYPE_G722;
    channel_options.audio_codec_opt.pcm_sample_rate = 16000;
    /* RTSA cloud AEC pairs microphone and reference data while PCM remains mono. */
    channel_options.audio_codec_opt.pcm_channel_num = 1;
    channel_options.audio_codec_opt.pcm_duration = MYBOT_AUDIO_PTIME_MS;

    int bwe_ret = agora_rtc_set_bwe_param(conn_id, 16000, 256000, 64000);
    if (bwe_ret < 0) {
        AOSL_LOG_WRN("set_bwe_param failed: %s", agora_rtc_err_2_str(bwe_ret));
    }

    s_rtc.conn_id = conn_id;
    set_state(MYBOT_RTC_STATE_CONNECTING);
    ret = agora_rtc_join_channel_with_user_account(
        conn_id, channel, user_account, token && token[0] ? token : NULL, &channel_options);
    if (ret < 0) {
        AOSL_LOG_ERR("join_channel failed: %s", agora_rtc_err_2_str(ret));
        s_rtc.conn_id = CONNECTION_ID_INVALID;
        int destroy_ret = agora_rtc_destroy_connection(conn_id);
        if (destroy_ret < 0) {
            AOSL_LOG_ERR("destroy_connection failed: %s", agora_rtc_err_2_str(destroy_ret));
            set_state(MYBOT_RTC_STATE_ERROR);
        } else {
            set_state(MYBOT_RTC_STATE_INITIALIZED);
        }
    } else {
        AOSL_LOG_NTC("[RTC] join requested (conn_id=%u, channel=%s, user=%s)", conn_id, channel,
                     user_account);
    }

out:
    rtc_unlock();
    if (ret < 0 && (rtm_started_for_join || rtm_subscription_started_for_join)) {
        (void)mybot_agora_rtc_logout_rtm();
    }
    return ret;
}

int mybot_agora_rtc_leave(void) {
    rtc_lock();

    connection_id_t conn_id = s_rtc.conn_id;
    if (conn_id == CONNECTION_ID_INVALID) {
        rtc_unlock();
        return mybot_agora_rtc_logout_rtm();
    }

    s_rtc.conn_id = CONNECTION_ID_INVALID;
    AOSL_LOG_NTC("[RTC] leaving connection (conn_id=%u)", conn_id);
    int leave_ret = agora_rtc_leave_channel(conn_id);
    if (leave_ret < 0) {
        AOSL_LOG_ERR("leave_channel failed: %s", agora_rtc_err_2_str(leave_ret));
    }
    int destroy_ret = agora_rtc_destroy_connection(conn_id);
    if (destroy_ret < 0) {
        AOSL_LOG_ERR("destroy_connection failed: %s", agora_rtc_err_2_str(destroy_ret));
    } else {
        AOSL_LOG_NTC("[RTC] connection destroyed (conn_id=%u)", conn_id);
    }
    set_state(destroy_ret < 0 ? MYBOT_RTC_STATE_ERROR : MYBOT_RTC_STATE_INITIALIZED);
    rtc_unlock();
    int rtm_ret = mybot_agora_rtc_logout_rtm();
    return destroy_ret < 0 || rtm_ret < 0 ? -1 : 0;
}

void mybot_agora_rtc_fini(void) {
    rtc_lock();
    if (s_rtc.fini_failed) {
        AOSL_LOG_ERR("[RTC] finalization not retried after an RTSA shutdown failure");
        rtc_unlock();
        return;
    }
    if (s_rtc.state == MYBOT_RTC_STATE_INIT_FAILED) {
        AOSL_LOG_WRN("[RTC] finalization skipped after initialization failure");
        rtc_unlock();
        return;
    }
    if (s_rtc.state == MYBOT_RTC_STATE_IDLE) {
        rtc_unlock();
        return;
    }
    rtc_unlock();

    (void)mybot_agora_rtc_leave();
    int ret = agora_rtc_fini();
    rtc_lock();
    if (ret < 0) {
        AOSL_LOG_ERR("agora_rtc_fini failed; RTC state retained: %s", agora_rtc_err_2_str(ret));
        s_rtc.state = MYBOT_RTC_STATE_ERROR;
        s_rtc.fini_failed = true;
        rtc_unlock();
        return;
    }

    clear_runtime_state();
    s_rtc.state = MYBOT_RTC_STATE_IDLE;
    AOSL_LOG_NTC("[RTC] Agora RTSA finalized");
    rtc_unlock();
}

int mybot_agora_rtc_send_audio(const void *data, size_t len) {
    if (!data || len == 0) {
        AOSL_LOG_ERR("[RTC] send_audio rejected: invalid PCM payload");
        return -1;
    }
    rtc_lock();

    int ret = -1;
    if (s_rtc.state == MYBOT_RTC_STATE_CONNECTED && s_rtc.conn_id != CONNECTION_ID_INVALID) {
        audio_frame_info_t info = {.data_type = AUDIO_DATA_TYPE_PCM};
        ret = agora_rtc_send_audio_data(s_rtc.conn_id, data, len, &info);
        if (ret < 0) {
            AOSL_LOG_ERR("send_audio failed: %s", agora_rtc_err_2_str(ret));
        }
    } else {
        AOSL_LOG_ERR("[RTC] send_audio rejected (state=%s, conn_id=%u)", state_name(s_rtc.state),
                     s_rtc.conn_id);
    }
    rtc_unlock();
    return ret;
}

int mybot_agora_rtc_renew_token(const char *token) {
    if (!token || !token[0]) {
        AOSL_LOG_ERR("[RTC] renew_token rejected: invalid token");
        return -1;
    }
    rtc_lock();

    int ret = -1;
    if (s_rtc.conn_id != CONNECTION_ID_INVALID) {
        ret = agora_rtc_renew_token(s_rtc.conn_id, token);
        if (ret < 0) {
            AOSL_LOG_ERR("renew_token failed: %s", agora_rtc_err_2_str(ret));
        }
    } else {
        AOSL_LOG_ERR("[RTC] renew_token rejected without an active connection");
    }
    rtc_unlock();
    return ret;
}
