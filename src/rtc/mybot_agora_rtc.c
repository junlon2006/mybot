/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_agora_rtc.h"

#include <mybot/mybot_build_config.h>

#include "agora_rtc_api.h"
#include <api/aosl_atomic.h>
#include <api/aosl_log.h>
#include <api/aosl_time.h>
#include <api/aosl_mpq.h>
#include <hal/aosl_hal_memory.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef MYBOT_RTM_LOGIN_TIMEOUT_MS
#define MYBOT_RTM_LOGIN_TIMEOUT_MS 5000U
#endif

#ifndef MYBOT_RTM_SUBSCRIBE_TIMEOUT_MS
#define MYBOT_RTM_SUBSCRIBE_TIMEOUT_MS 5000U
#endif

#define MYBOT_RTM_LOGIN_POLL_MS 10U

#define MYBOT_RTC_PCM_SAMPLE_RATE 16000U
#define MYBOT_RTC_PCM_CHANNELS 1U
#define MYBOT_RTC_PCM_BYTES_PER_SAMPLE sizeof(int16_t)
#define MYBOT_RTC_PCM_FRAME_BYTES                                                                  \
    (MYBOT_RTC_PCM_SAMPLE_RATE * MYBOT_AUDIO_PTIME_MS / 1000U * MYBOT_RTC_PCM_CHANNELS *           \
     MYBOT_RTC_PCM_BYTES_PER_SAMPLE)

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
} mybot_agora_rtc_t;

static mybot_agora_rtc_t s_rtc = {.conn_id = CONNECTION_ID_INVALID};
static aosl_atomic_t s_rtc_mpq_id = AOSL_MPQ_INVALID;
static aosl_atomic_t s_rtm_login_done;
static aosl_atomic_t s_rtm_login_ok;
static aosl_atomic_t s_rtm_sub_done;
static aosl_atomic_t s_rtm_sub_ok;
static aosl_atomic_t s_rtc_callbacks_enabled;

static void clear_rtm_subscription(void);
static void clear_rtm_login(void);

static void process_rtm_event(const char *, rtm_event_type_e, rtm_err_code_e);
static void process_rtm_data(const char *, const void *, size_t, rtm_message_type_e, const char *);
static void process_rtm_subscribe_result(const char *, rtm_err_code_e);
static void process_rtm_subscribe_data(const char *, const char *, const void *, size_t,
                                       rtm_message_type_e, const char *);
static void process_rtm_send_data_result(const char *, uint32_t, rtm_msg_state_e);
static void process_join_channel_success(connection_id_t, uint32_t, int);
static void process_reconnecting(connection_id_t);
static void process_connection_lost(connection_id_t);
static void process_rejoin_channel_success(connection_id_t, uint32_t, int);
static void process_user_joined(connection_id_t, const user_info_t *, int);
static void process_user_offline(connection_id_t, const user_info_t *, int);
static void process_audio_data(connection_id_t, uint32_t, uint16_t, const void *, size_t,
                               const audio_frame_info_t *);
static void process_error(connection_id_t, int, const char *);
static void process_license_failed(connection_id_t, int);
static void process_token_will_expire(connection_id_t, const char *);
static void on_rtm_event(const char *, rtm_event_type_e, rtm_err_code_e);
static void on_rtm_data(const char *, const void *, size_t, rtm_message_type_e, const char *);
static void on_rtm_subscribe_result(const char *, rtm_err_code_e);
static void on_rtm_subscribe_data(const char *, const char *, const void *, size_t,
                                  rtm_message_type_e, const char *);
static void on_rtm_send_data_result(const char *, uint32_t, rtm_msg_state_e);
static void on_join_channel_success(connection_id_t, uint32_t, int);
static void on_reconnecting(connection_id_t);
static void on_connection_lost(connection_id_t);
static void on_rejoin_channel_success(connection_id_t, uint32_t, int);
static void on_user_joined(connection_id_t, const user_info_t *, int);
static void on_user_offline(connection_id_t, const user_info_t *, int);
static void on_audio_data(connection_id_t, uint32_t, uint16_t, const void *, size_t,
                          const audio_frame_info_t *);
static void on_error(connection_id_t, int, const char *);
static void on_license_failed(connection_id_t, int);
static void on_token_will_expire(connection_id_t, const char *);

typedef enum {
    RTC_EVENT_RTM_EVENT,
    RTC_EVENT_RTM_DATA,
    RTC_EVENT_RTM_SUB_RESULT,
    RTC_EVENT_RTM_SUB_DATA,
    RTC_EVENT_RTM_SEND_RESULT,
    RTC_EVENT_JOIN_SUCCESS,
    RTC_EVENT_RECONNECTING,
    RTC_EVENT_CONNECTION_LOST,
    RTC_EVENT_REJOIN_SUCCESS,
    RTC_EVENT_USER_JOINED,
    RTC_EVENT_USER_OFFLINE,
    RTC_EVENT_AUDIO,
    RTC_EVENT_ERROR,
    RTC_EVENT_LICENSE,
    RTC_EVENT_TOKEN,
} rtc_event_type_t;

typedef struct {
    rtc_event_type_t type;
    connection_id_t conn_id;
    uint32_t uid;
    int value;
    rtm_event_type_e rtm_event;
    rtm_err_code_e rtm_error;
    rtm_message_type_e message_type;
    rtm_msg_state_e message_state;
    size_t len;
    uint16_t sent_ts;
    audio_frame_info_t audio_info;
    bool has_audio_info;
    char text[AGORA_RTC_CHANNEL_NAME_MAX_LEN + 1];
    char uid_text[MYBOT_RTM_UID_MAX_LEN];
    char custom_type[33];
    user_info_t user;
    void *data;
} rtc_event_t;

static void rtc_event_free(rtc_event_t *event) {
    if (event) {
        aosl_hal_free(event->data);
        aosl_hal_free(event);
    }
}

static void rtc_event_process(const aosl_ts_t *ts, aosl_refobj_t ref, uintptr_t argc,
                              uintptr_t argv[]) {
    (void)ts;
    (void)ref;
    if (argc != 1 || !argv[0]) {
        if (argc == 1 && argv[0])
            rtc_event_free((rtc_event_t *)argv[0]);
        return;
    }
    rtc_event_t *e = (rtc_event_t *)argv[0];
    switch (e->type) {
    case RTC_EVENT_RTM_EVENT:
        process_rtm_event(e->uid_text, e->rtm_event, e->rtm_error);
        break;
    case RTC_EVENT_RTM_DATA:
        process_rtm_data(e->uid_text, e->data, e->len, e->message_type,
                         e->custom_type[0] ? e->custom_type : NULL);
        break;
    case RTC_EVENT_RTM_SUB_RESULT:
        process_rtm_subscribe_result(e->text, e->rtm_error);
        break;
    case RTC_EVENT_RTM_SUB_DATA:
        process_rtm_subscribe_data(e->text, e->uid_text, e->data, e->len, e->message_type,
                                   e->custom_type[0] ? e->custom_type : NULL);
        break;
    case RTC_EVENT_RTM_SEND_RESULT:
        process_rtm_send_data_result(e->uid_text, e->uid, e->message_state);
        break;
    case RTC_EVENT_JOIN_SUCCESS:
        process_join_channel_success(e->conn_id, e->uid, e->value);
        break;
    case RTC_EVENT_RECONNECTING:
        process_reconnecting(e->conn_id);
        break;
    case RTC_EVENT_CONNECTION_LOST:
        process_connection_lost(e->conn_id);
        break;
    case RTC_EVENT_REJOIN_SUCCESS:
        process_rejoin_channel_success(e->conn_id, e->uid, e->value);
        break;
    case RTC_EVENT_USER_JOINED:
        process_user_joined(e->conn_id, &e->user, e->value);
        break;
    case RTC_EVENT_USER_OFFLINE:
        process_user_offline(e->conn_id, &e->user, e->value);
        break;
    case RTC_EVENT_AUDIO:
        process_audio_data(e->conn_id, e->uid, e->sent_ts, e->data, e->len,
                           e->has_audio_info ? &e->audio_info : NULL);
        break;
    case RTC_EVENT_ERROR:
        process_error(e->conn_id, e->value, e->text);
        break;
    case RTC_EVENT_LICENSE:
        process_license_failed(e->conn_id, e->value);
        break;
    case RTC_EVENT_TOKEN:
        process_token_will_expire(e->conn_id, NULL);
        break;
    }
    rtc_event_free(e);
}

static bool rtc_event_queue(rtc_event_t *event) {
    aosl_mpq_t q = (aosl_mpq_t)aosl_atomic_read(&s_rtc_mpq_id);
    if (!event || q == AOSL_MPQ_INVALID || !aosl_atomic_read(&s_rtc_callbacks_enabled)) {
        rtc_event_free(event);
        return false;
    }
    /* The queue is NONBLOCK so RTSA teardown can drain its callback worker
     * without waiting for this worker. Audio and stale notifications may be
     * dropped when the bounded queue is full. */
    if (aosl_mpq_queue(q, AOSL_MPQ_INVALID, AOSL_REF_INVALID, "rtc_callback", rtc_event_process, 1,
                       (uintptr_t)event) < 0) {
        rtc_event_free(event);
        return false;
    }
    return true;
}

static rtc_event_t *rtc_event_new(rtc_event_type_t type) {
    rtc_event_t *event = (rtc_event_t *)aosl_hal_malloc(sizeof(*event));
    if (event) {
        memset(event, 0, sizeof(*event));
        event->type = type;
    }
    return event;
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

/* All state transitions and application callbacks run on the RTC MPQ worker. */
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

static void process_rtm_event(const char *rtm_uid, rtm_event_type_e event_type,
                              rtm_err_code_e error_code) {
    mybot_rtm_event_type_t mapped_event;

    if (!s_rtc.rtm_login_requested) {
        AOSL_LOG_NTC("[RTM] event ignored: login was not requested (type=%s(%d), error=%d)",
                     rtm_event_name(event_type), (int)event_type, (int)error_code);
        return;
    }
    if (!rtm_uid) {
        AOSL_LOG_WRN("[RTM] event ignored: missing UID (type=%s(%d), error=%d)",
                     rtm_event_name(event_type), (int)event_type, (int)error_code);
        return;
    }
    if (strcmp(rtm_uid, s_rtc.rtm_uid) != 0) {
        AOSL_LOG_WRN("[RTM] event ignored: UID mismatch (expected=%s, received=%s, type=%s(%d))",
                     s_rtc.rtm_uid, rtm_uid, rtm_event_name(event_type), (int)event_type);
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
            clear_rtm_login();
            AOSL_LOG_WRN("[RTM] login failed (uid=%s, error=%d)", rtm_uid, (int)error_code);
        }
        break;
    case RTM_EVENT_TYPE_KICKOFF:
        clear_rtm_login();
        AOSL_LOG_WRN("[RTM] account kicked off (uid=%s, error=%d)", rtm_uid, (int)error_code);
        break;
    case RTM_EVENT_TYPE_EXIT:
        clear_rtm_login();
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
}

static void process_rtm_data(const char *rtm_uid, const void *data, size_t len,
                             rtm_message_type_e message_type, const char *custom_type) {
    size_t preview_len = len > 512U ? 512U : len;

    if (!s_rtc.rtm_login_requested) {
        AOSL_LOG_NTC(
            "[RTM] data ignored: login was not requested (from=%s, message_type=%s, type=%s, "
            "len=%zu)",
            rtm_uid ? rtm_uid : "(null)", rtm_message_type_name(message_type),
            custom_type ? custom_type : "(null)", len);
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
}

static void process_rtm_subscribe_result(const char *channel, rtm_err_code_e error_code) {
    if (!s_rtc.rtm_subscribe_requested) {
        AOSL_LOG_NTC("[RTM] subscribe result ignored: no subscription requested (channel=%s, "
                     "error=%d)",
                     channel ? channel : "(null)", (int)error_code);
        return;
    }
    if (!channel || strcmp(channel, s_rtc.rtm_channel) != 0) {
        AOSL_LOG_WRN("[RTM] subscribe result ignored: channel mismatch (expected=%s, received=%s)",
                     s_rtc.rtm_channel, channel ? channel : "(null)");
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
}

static void process_rtm_subscribe_data(const char *channel, const char *rtm_uid, const void *data,
                                       size_t len, rtm_message_type_e message_type,
                                       const char *custom_type) {
    size_t preview_len = len > 512U ? 512U : len;

    if (!s_rtc.rtm_subscribed) {
        AOSL_LOG_NTC("[RTM] channel data ignored: no active subscription (channel=%s, from=%s, "
                     "len=%zu)",
                     channel ? channel : "(null)", rtm_uid ? rtm_uid : "(null)", len);
        return;
    }
    if (!channel || strcmp(channel, s_rtc.rtm_channel) != 0) {
        AOSL_LOG_WRN("[RTM] channel data ignored: channel mismatch (expected=%s, received=%s)",
                     s_rtc.rtm_channel, channel ? channel : "(null)");
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
}

static void process_rtm_send_data_result(const char *rtm_uid, uint32_t msg_id,
                                         rtm_msg_state_e state) {
    if (s_rtc.rtm_login_requested && s_rtc.callbacks.on_rtm_send_data_result) {
        s_rtc.callbacks.on_rtm_send_data_result(rtm_uid, msg_id, (mybot_rtm_message_state_t)state,
                                                s_rtc.callbacks.user_data);
    }
}

static int login_rtm(const char *rtm_uid, const char *rtm_token) {
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
    aosl_atomic_set(&s_rtm_login_done, false);
    aosl_atomic_set(&s_rtm_login_ok, false);
    int ret = agora_rtm_login(rtm_uid, rtm_token && rtm_token[0] ? rtm_token : NULL, &handler);
    if (ret < 0) {
        AOSL_LOG_ERR("[RTM] login failed: %s", agora_rtc_err_2_str(ret));
        clear_rtm_login();
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
        bool request_matches = s_rtc.rtm_login_requested && strcmp(s_rtc.rtm_uid, rtm_uid) == 0;
        bool completed = aosl_atomic_read(&s_rtm_login_done);
        bool logged_in = aosl_atomic_read(&s_rtm_login_ok);

        if (!request_matches) {
            AOSL_LOG_ERR("[RTM] login wait aborted: request is no longer active (uid=%s)", rtm_uid);
            return -1;
        }
        if (completed) {
            s_rtc.rtm_login_completed = true;
            s_rtc.rtm_logged_in = logged_in;
            if (!logged_in) {
                clear_rtm_login();
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

static void clear_rtm_subscription(void) {
    s_rtc.rtm_channel[0] = '\0';
    s_rtc.rtm_subscribe_requested = false;
    s_rtc.rtm_subscribe_completed = false;
    s_rtc.rtm_subscribed = false;
}

static void clear_rtm_login(void) {
    clear_rtm_subscription();
    s_rtc.rtm_uid[0] = '\0';
    s_rtc.rtm_login_requested = false;
    s_rtc.rtm_login_completed = false;
    s_rtc.rtm_logged_in = false;
}

static int unsubscribe_rtm(void) {
    if (!s_rtc.rtm_subscribe_requested) {
        clear_rtm_subscription();
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
    clear_rtm_subscription();
    return ret;
}

static int subscribe_rtm(const char *channel) {
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
    aosl_atomic_set(&s_rtm_sub_done, false);
    aosl_atomic_set(&s_rtm_sub_ok, false);
    int ret = agora_rtm_subscribe(channel);
    if (ret < 0) {
        AOSL_LOG_ERR("[RTM] subscribe failed: %s", agora_rtc_err_2_str(ret));
        clear_rtm_subscription();
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
        bool request_matches =
            s_rtc.rtm_subscribe_requested && strcmp(s_rtc.rtm_channel, channel) == 0;
        bool completed = aosl_atomic_read(&s_rtm_sub_done);
        bool subscribed = aosl_atomic_read(&s_rtm_sub_ok);

        if (!request_matches) {
            AOSL_LOG_ERR("[RTM] subscription wait aborted: request is no longer active "
                         "(channel=%s)",
                         channel);
            return -1;
        }
        if (completed) {
            s_rtc.rtm_subscribe_completed = true;
            s_rtc.rtm_subscribed = subscribed;
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

static void process_join_channel_success(connection_id_t conn_id, uint32_t uid, int elapsed) {
    if (!connection_is_active(conn_id)) {
        return;
    }
    AOSL_LOG_NTC("join channel success (uid=%u, elapsed=%d ms)", uid, elapsed);
    set_state(MYBOT_RTC_STATE_CONNECTED);
}

static void process_reconnecting(connection_id_t conn_id) {
    if (!connection_is_active(conn_id)) {
        return;
    }
    set_state(MYBOT_RTC_STATE_RECONNECTING);
}

static void process_connection_lost(connection_id_t conn_id) {
    if (!connection_is_active(conn_id)) {
        return;
    }
    set_state(MYBOT_RTC_STATE_DISCONNECTED);
}

static void process_rejoin_channel_success(connection_id_t conn_id, uint32_t uid, int elapsed_ms) {
    (void)uid;
    (void)elapsed_ms;
    if (!connection_is_active(conn_id)) {
        return;
    }
    set_state(MYBOT_RTC_STATE_CONNECTED);
}

static void process_user_joined(connection_id_t conn_id, const user_info_t *user, int elapsed_ms) {
    (void)elapsed_ms;
    if (!user) {
        return;
    }
    if (!connection_is_active(conn_id)) {
        return;
    }
    AOSL_LOG_NTC("[RTC] user \"%s\" (uid=%u) joined", user->user_account, user->uid);
}

static void process_user_offline(connection_id_t conn_id, const user_info_t *user, int reason) {
    if (!user) {
        return;
    }
    if (!connection_is_active(conn_id)) {
        return;
    }
    AOSL_LOG_NTC("[RTC] user \"%s\" (uid=%u) offline (reason=%d)", user->user_account, user->uid,
                 reason);
}

static void process_audio_data(connection_id_t conn_id, uint32_t uid, uint16_t sent_ts,
                               const void *data, size_t len, const audio_frame_info_t *info) {
    (void)sent_ts;
    if (!connection_is_active(conn_id)) {
        return;
    }
    /* The selected RTSA build emits one decoded PCM frame per callback; its
     * frame duration is tied to MYBOT_AUDIO_PTIME_MS at configuration time. */
    if (!data || !info || info->data_type != AUDIO_DATA_TYPE_PCM || len == 0 ||
        (len % MYBOT_RTC_PCM_BYTES_PER_SAMPLE) != 0 || len != MYBOT_RTC_PCM_FRAME_BYTES) {
        AOSL_LOG_WRN(
            "[RTC] dropped invalid downlink audio (len=%zu, info=%s, type=%d, expected=%u)", len,
            info ? "present" : "missing", info ? (int)info->data_type : -1,
            (unsigned int)MYBOT_RTC_PCM_FRAME_BYTES);
        return;
    }
    if (s_rtc.callbacks.on_remote_audio) {
        s_rtc.callbacks.on_remote_audio(uid, data, len, s_rtc.callbacks.user_data);
    }
}

static void process_error(connection_id_t conn_id, int code, const char *message) {
    if (conn_id == CONNECTION_ID_ALL) {
        AOSL_LOG_ERR("[RTC] global error without connection ownership (code=%d): %s", code,
                     message ? message : "null");
        return;
    }
    if (!connection_is_active(conn_id)) {
        return;
    }
    AOSL_LOG_ERR("[RTC] error (code=%d): %s", code, message ? message : "null");
    set_state(MYBOT_RTC_STATE_ERROR);
}

static void process_license_failed(connection_id_t conn_id, int reason) {
    if (!connection_is_active(conn_id)) {
        return;
    }
    AOSL_LOG_ERR("[RTC] license validation failed (reason=%d)", reason);
    set_state(MYBOT_RTC_STATE_ERROR);
}

static void process_token_will_expire(connection_id_t conn_id, const char *token) {
    (void)token;
    if (!connection_is_active(conn_id)) {
        return;
    }
    AOSL_LOG_NTC("[RTC] token privilege will expire");
    if (s_rtc.callbacks.on_token_will_expire) {
        s_rtc.callbacks.on_token_will_expire(s_rtc.callbacks.user_data);
    }
}

/* RTSA invokes these handlers on its callback worker. They only copy the
 * borrowed callback arguments and enqueue a non-blocking event for rtc_mpq. */
static void on_rtm_event(const char *uid, rtm_event_type_e type, rtm_err_code_e error) {
    if (!aosl_atomic_read(&s_rtc_callbacks_enabled))
        return;
    if (type == RTM_EVENT_TYPE_LOGIN) {
        aosl_atomic_set(&s_rtm_login_ok, error == ERR_RTM_OK);
        aosl_atomic_set(&s_rtm_login_done, true);
    }
    rtc_event_t *e = rtc_event_new(RTC_EVENT_RTM_EVENT);
    if (!e || !uid || strlen(uid) >= sizeof(e->uid_text)) {
        rtc_event_free(e);
        return;
    }
    memcpy(e->uid_text, uid, strlen(uid) + 1U);
    e->rtm_event = type;
    e->rtm_error = error;
    (void)rtc_event_queue(e);
}
static void on_rtm_data(const char *uid, const void *data, size_t len, rtm_message_type_e type,
                        const char *custom) {
    if (!uid || !data || len > AGORA_RTM_DATA_MAX_LEN)
        return;
    rtc_event_t *e = rtc_event_new(RTC_EVENT_RTM_DATA);
    if (!e || strlen(uid) >= sizeof(e->uid_text) ||
        (custom && strlen(custom) >= sizeof(e->custom_type))) {
        rtc_event_free(e);
        return;
    }
    memcpy(e->uid_text, uid, strlen(uid) + 1U);
    if (custom)
        memcpy(e->custom_type, custom, strlen(custom) + 1U);
    e->message_type = type;
    e->len = len;
    e->data = aosl_hal_malloc(len);
    if (!e->data) {
        rtc_event_free(e);
        return;
    }
    memcpy(e->data, data, len);
    (void)rtc_event_queue(e);
}
static void on_rtm_subscribe_result(const char *channel, rtm_err_code_e error) {
    if (!aosl_atomic_read(&s_rtc_callbacks_enabled))
        return;
    aosl_atomic_set(&s_rtm_sub_ok, error == ERR_RTM_OK);
    aosl_atomic_set(&s_rtm_sub_done, true);
    if (!channel || strlen(channel) >= sizeof(((rtc_event_t *)0)->text))
        return;
    rtc_event_t *e = rtc_event_new(RTC_EVENT_RTM_SUB_RESULT);
    if (!e)
        return;
    memcpy(e->text, channel, strlen(channel) + 1U);
    e->rtm_error = error;
    (void)rtc_event_queue(e);
}
static void on_rtm_subscribe_data(const char *channel, const char *uid, const void *data,
                                  size_t len, rtm_message_type_e type, const char *custom) {
    if (!channel || !uid || !data || len > AGORA_RTM_DATA_MAX_LEN)
        return;
    rtc_event_t *e = rtc_event_new(RTC_EVENT_RTM_SUB_DATA);
    if (!e || strlen(channel) >= sizeof(e->text) || strlen(uid) >= sizeof(e->uid_text) ||
        (custom && strlen(custom) >= sizeof(e->custom_type))) {
        rtc_event_free(e);
        return;
    }
    memcpy(e->text, channel, strlen(channel) + 1U);
    memcpy(e->uid_text, uid, strlen(uid) + 1U);
    if (custom)
        memcpy(e->custom_type, custom, strlen(custom) + 1U);
    e->message_type = type;
    e->len = len;
    e->data = aosl_hal_malloc(len);
    if (!e->data) {
        rtc_event_free(e);
        return;
    }
    memcpy(e->data, data, len);
    (void)rtc_event_queue(e);
}
static void on_rtm_send_data_result(const char *uid, uint32_t id, rtm_msg_state_e state) {
    if (!uid || strlen(uid) >= sizeof(((rtc_event_t *)0)->uid_text))
        return;
    rtc_event_t *e = rtc_event_new(RTC_EVENT_RTM_SEND_RESULT);
    if (!e)
        return;
    memcpy(e->uid_text, uid, strlen(uid) + 1U);
    e->uid = id;
    e->message_state = state;
    (void)rtc_event_queue(e);
}
static void on_join_channel_success(connection_id_t c, uint32_t uid, int elapsed) {
    rtc_event_t *e = rtc_event_new(RTC_EVENT_JOIN_SUCCESS);
    if (!e)
        return;
    e->conn_id = c;
    e->uid = uid;
    e->value = elapsed;
    (void)rtc_event_queue(e);
}
static void on_reconnecting(connection_id_t c) {
    rtc_event_t *e = rtc_event_new(RTC_EVENT_RECONNECTING);
    if (e) {
        e->conn_id = c;
        (void)rtc_event_queue(e);
    }
}
static void on_connection_lost(connection_id_t c) {
    rtc_event_t *e = rtc_event_new(RTC_EVENT_CONNECTION_LOST);
    if (e) {
        e->conn_id = c;
        (void)rtc_event_queue(e);
    }
}
static void on_rejoin_channel_success(connection_id_t c, uint32_t uid, int elapsed) {
    rtc_event_t *e = rtc_event_new(RTC_EVENT_REJOIN_SUCCESS);
    if (e) {
        e->conn_id = c;
        e->uid = uid;
        e->value = elapsed;
        (void)rtc_event_queue(e);
    }
}
static void on_user_joined(connection_id_t c, const user_info_t *u, int elapsed) {
    if (!u)
        return;
    rtc_event_t *e = rtc_event_new(RTC_EVENT_USER_JOINED);
    if (e) {
        e->conn_id = c;
        e->user = *u;
        e->value = elapsed;
        (void)rtc_event_queue(e);
    }
}
static void on_user_offline(connection_id_t c, const user_info_t *u, int reason) {
    if (!u)
        return;
    rtc_event_t *e = rtc_event_new(RTC_EVENT_USER_OFFLINE);
    if (e) {
        e->conn_id = c;
        e->user = *u;
        e->value = reason;
        (void)rtc_event_queue(e);
    }
}
static void on_audio_data(connection_id_t c, uint32_t uid, uint16_t ts, const void *d, size_t len,
                          const audio_frame_info_t *i) {
    if (!d || !i || i->data_type != AUDIO_DATA_TYPE_PCM || len == 0 ||
        (len % MYBOT_RTC_PCM_BYTES_PER_SAMPLE) != 0 || len != MYBOT_RTC_PCM_FRAME_BYTES) {
        AOSL_LOG_WRN("[RTC] ignored invalid downlink audio callback (len=%zu, info=%s, type=%d, "
                     "expected=%u)",
                     len, i ? "present" : "missing", i ? (int)i->data_type : -1,
                     (unsigned int)MYBOT_RTC_PCM_FRAME_BYTES);
        return;
    }
    rtc_event_t *e = rtc_event_new(RTC_EVENT_AUDIO);
    if (e) {
        e->conn_id = c;
        e->uid = uid;
        e->sent_ts = ts;
        if (i) {
            e->audio_info = *i;
            e->has_audio_info = true;
        }
        e->len = len;
        e->data = aosl_hal_malloc(len);
        if (!e->data) {
            rtc_event_free(e);
            return;
        }
        memcpy(e->data, d, len);
        (void)rtc_event_queue(e);
    }
}
static void on_error(connection_id_t c, int code, const char *m) {
    rtc_event_t *e = rtc_event_new(RTC_EVENT_ERROR);
    if (e) {
        e->conn_id = c;
        e->value = code;
        if (m)
            snprintf(e->text, sizeof(e->text), "%s", m);
        (void)rtc_event_queue(e);
    }
}
static void on_license_failed(connection_id_t c, int reason) {
    rtc_event_t *e = rtc_event_new(RTC_EVENT_LICENSE);
    if (e) {
        e->conn_id = c;
        e->value = reason;
        (void)rtc_event_queue(e);
    }
}
static void on_token_will_expire(connection_id_t c, const char *t) {
    (void)t;
    rtc_event_t *e = rtc_event_new(RTC_EVENT_TOKEN);
    if (e) {
        e->conn_id = c;
        (void)rtc_event_queue(e);
    }
}

static void clear_runtime_state(void) {
    s_rtc.conn_id = CONNECTION_ID_INVALID;
    memset(&s_rtc.callbacks, 0, sizeof(s_rtc.callbacks));
    s_rtc.app_id[0] = '\0';
    clear_rtm_login();
}

static int rtc_init_impl(const char *app_id, const mybot_agora_rtc_callbacks_t *callbacks) {
    if (!app_id || !app_id[0] || strlen(app_id) >= sizeof(s_rtc.app_id)) {
        AOSL_LOG_ERR("[RTC] init rejected: invalid App ID");
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
        /* RTSA init failure leaves this wrapper uninitialized. The exact
         * package must guarantee that a failed init has no live service before
         * retrying; no fini is issued here because the vendor API does not
         * define fini-after-failed-init semantics. */
        s_rtc.state = MYBOT_RTC_STATE_IDLE;
        return -1;
    }

    s_rtc.state = MYBOT_RTC_STATE_INITIALIZED;
    AOSL_LOG_NTC("agora_rtc_init ok (sdk v%s)", agora_rtc_get_version());
    return 0;
}

static int rtc_login_rtm_impl(const char *rtm_uid, const char *rtm_token) {
    int ret = login_rtm(rtm_uid, rtm_token);
    return ret;
}

static int rtc_logout_rtm_impl(void) {
    if (!s_rtc.rtm_login_requested) {
        clear_rtm_subscription();
        return 0;
    }

    int unsubscribe_ret = unsubscribe_rtm();
    int ret = agora_rtm_logout();
    if (ret < 0) {
        AOSL_LOG_ERR("[RTM] logout failed: %s", agora_rtc_err_2_str(ret));
        return -1;
    }
    AOSL_LOG_NTC("[RTM] logged out (uid=%s)", s_rtc.rtm_uid);
    clear_rtm_login();
    return unsubscribe_ret < 0 ? -1 : 0;
}

static bool rtc_is_rtm_logged_in_impl(void) {
    bool logged_in = s_rtc.rtm_logged_in;
    return logged_in;
}

static int rtc_send_rtm_data_impl(const char *peer_rtm_uid, const void *data, size_t len,
                                  uint32_t msg_id, const char *custom_type) {
    if (!mybot_agora_rtc_rtm_uid_is_valid(peer_rtm_uid) || !data || len == 0 || len > 31U * 1024U ||
        (custom_type && strlen(custom_type) > 32U)) {
        AOSL_LOG_ERR("[RTM] send rejected: invalid peer, payload, or custom type");
        return -1;
    }

    if (!s_rtc.rtm_logged_in) {
        AOSL_LOG_ERR("[RTM] send rejected: login has not completed");
        return -1;
    }
    int ret =
        agora_rtm_send_data(peer_rtm_uid, data, len, msg_id, RTM_MESSAGE_TYPE_BINARY, custom_type);
    if (ret < 0) {
        AOSL_LOG_ERR("[RTM] send failed: %s", agora_rtc_err_2_str(ret));
    }
    return ret;
}

static int rtc_join_impl(const char *channel, const char *token, const char *user_account) {
    if (!channel || !channel[0] || strlen(channel) >= AGORA_RTC_CHANNEL_NAME_MAX_LEN ||
        !user_account || !user_account[0]) {
        AOSL_LOG_ERR("[RTC] join rejected: invalid channel or user account");
        return -1;
    }
    bool rtm_started_for_join = false;
    if (s_rtc.rtm_login_requested) {
        if (strcmp(s_rtc.rtm_uid, user_account) != 0) {
            AOSL_LOG_ERR("[RTC] join rejected: RTM UID does not match RTC user account");
            return -1;
        }
    } else {
        if (login_rtm(user_account, token) < 0) {
            return -1;
        }
        rtm_started_for_join = true;
    }

    if (wait_for_rtm_login(user_account) < 0) {
        if (rtm_started_for_join) {
            (void)rtc_logout_rtm_impl();
        }
        return -1;
    }

    bool rtm_subscription_started_for_join = false;
    int ret = -1;
    if (s_rtc.state != MYBOT_RTC_STATE_INITIALIZED || s_rtc.conn_id != CONNECTION_ID_INVALID ||
        !s_rtc.rtm_logged_in || strcmp(s_rtc.rtm_uid, user_account) != 0) {
        AOSL_LOG_ERR("[RTC] join rejected after RTM wait (state=%s, conn_id=%u, rtm_logged_in=%d)",
                     state_name(s_rtc.state), s_rtc.conn_id, s_rtc.rtm_logged_in ? 1 : 0);
        goto out;
    }
    rtm_subscription_started_for_join = !s_rtc.rtm_subscribe_requested;
    if (subscribe_rtm(channel) < 0) {
        goto out;
    }

    if (wait_for_rtm_subscription(channel) < 0) {
        (void)rtc_logout_rtm_impl();
        return -1;
    }

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
    if (ret < 0 && (rtm_started_for_join || rtm_subscription_started_for_join)) {
        (void)rtc_logout_rtm_impl();
    }
    return ret;
}

static int rtc_leave_impl(void) {

    connection_id_t conn_id = s_rtc.conn_id;
    if (conn_id == CONNECTION_ID_INVALID) {
        return rtc_logout_rtm_impl();
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
    int rtm_ret = rtc_logout_rtm_impl();
    return leave_ret < 0 || destroy_ret < 0 || rtm_ret < 0 ? -1 : 0;
}

static int rtc_fini_impl(void) {
    if (s_rtc.state == MYBOT_RTC_STATE_IDLE) {
        return 0;
    }

    int leave_ret = rtc_leave_impl();
    int ret = agora_rtc_fini();
    if (ret < 0) {
        AOSL_LOG_ERR("agora_rtc_fini failed; RTC state retained: %s", agora_rtc_err_2_str(ret));
        s_rtc.state = MYBOT_RTC_STATE_ERROR;
        return -1;
    }

    clear_runtime_state();
    s_rtc.state = MYBOT_RTC_STATE_IDLE;
    AOSL_LOG_NTC("[RTC] Agora RTSA finalized");
    return leave_ret < 0 ? -1 : 0;
}

static int rtc_send_audio_impl(const void *data, size_t len) {
    if (!data || len == 0) {
        AOSL_LOG_ERR("[RTC] send_audio rejected: invalid PCM payload");
        return -1;
    }

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
    return ret;
}

static int rtc_renew_token_impl(const char *token) {
    if (!token || !token[0]) {
        AOSL_LOG_ERR("[RTC] renew_token rejected: invalid token");
        return -1;
    }

    int ret = -1;
    if (s_rtc.conn_id != CONNECTION_ID_INVALID) {
        ret = agora_rtc_renew_token(s_rtc.conn_id, token);
        if (ret < 0) {
            AOSL_LOG_ERR("renew_token failed: %s", agora_rtc_err_2_str(ret));
        }
    } else {
        AOSL_LOG_ERR("[RTC] renew_token rejected without an active connection");
    }
    return ret;
}

typedef void (*rtc_mpq_func_t)(const aosl_ts_t *, aosl_refobj_t, uintptr_t, uintptr_t[]);

static int rtc_call(const char *name, rtc_mpq_func_t func, uintptr_t argc, uintptr_t argv[]) {
    aosl_mpq_t q = (aosl_mpq_t)aosl_atomic_read(&s_rtc_mpq_id);
    if (q == AOSL_MPQ_INVALID) {
        return -1;
    }
    return aosl_mpq_call_argv(q, AOSL_REF_INVALID, name, func, argc, argv);
}

static void cmd_init(const aosl_ts_t *ts, aosl_refobj_t ref, uintptr_t argc, uintptr_t argv[]) {
    (void)ts;
    (void)ref;
    if (argc == 3) {
        *(int *)argv[0] =
            rtc_init_impl((const char *)argv[1], (const mybot_agora_rtc_callbacks_t *)argv[2]);
    }
}
static void cmd_login(const aosl_ts_t *ts, aosl_refobj_t ref, uintptr_t argc, uintptr_t argv[]) {
    (void)ts;
    (void)ref;
    if (argc == 3)
        *(int *)argv[0] = rtc_login_rtm_impl((const char *)argv[1], (const char *)argv[2]);
}
static void cmd_logout(const aosl_ts_t *ts, aosl_refobj_t ref, uintptr_t argc, uintptr_t argv[]) {
    (void)ts;
    (void)ref;
    if (argc == 1)
        *(int *)argv[0] = rtc_logout_rtm_impl();
}
static void cmd_is_logged(const aosl_ts_t *ts, aosl_refobj_t ref, uintptr_t argc,
                          uintptr_t argv[]) {
    (void)ts;
    (void)ref;
    if (argc == 1)
        *(bool *)argv[0] = rtc_is_rtm_logged_in_impl();
}
static void cmd_send_rtm(const aosl_ts_t *ts, aosl_refobj_t ref, uintptr_t argc, uintptr_t argv[]) {
    (void)ts;
    (void)ref;
    if (argc == 6)
        *(int *)argv[0] =
            rtc_send_rtm_data_impl((const char *)argv[1], (const void *)argv[2], (size_t)argv[3],
                                   (uint32_t)argv[4], (const char *)argv[5]);
}
static void cmd_join(const aosl_ts_t *ts, aosl_refobj_t ref, uintptr_t argc, uintptr_t argv[]) {
    (void)ts;
    (void)ref;
    if (argc == 4)
        *(int *)argv[0] =
            rtc_join_impl((const char *)argv[1], (const char *)argv[2], (const char *)argv[3]);
}
static void cmd_leave(const aosl_ts_t *ts, aosl_refobj_t ref, uintptr_t argc, uintptr_t argv[]) {
    (void)ts;
    (void)ref;
    if (argc == 1)
        *(int *)argv[0] = rtc_leave_impl();
}
static void cmd_fini(const aosl_ts_t *ts, aosl_refobj_t ref, uintptr_t argc, uintptr_t argv[]) {
    (void)ts;
    (void)ref;
    (void)argc;
    (void)argv;
    aosl_atomic_set(&s_rtc_callbacks_enabled, false);
    if (argc == 1) {
        *(int *)argv[0] = rtc_fini_impl();
        if (*(int *)argv[0] < 0)
            aosl_atomic_set(&s_rtc_callbacks_enabled, true);
    }
}
static void cmd_barrier(const aosl_ts_t *ts, aosl_refobj_t ref, uintptr_t argc, uintptr_t argv[]) {
    (void)ts;
    (void)ref;
    (void)argc;
    (void)argv;
}
static void cmd_send_audio(const aosl_ts_t *ts, aosl_refobj_t ref, uintptr_t argc,
                           uintptr_t argv[]) {
    (void)ts;
    (void)ref;
    if (argc == 3)
        *(int *)argv[0] = rtc_send_audio_impl((const void *)argv[1], (size_t)argv[2]);
}
static void cmd_renew(const aosl_ts_t *ts, aosl_refobj_t ref, uintptr_t argc, uintptr_t argv[]) {
    (void)ts;
    (void)ref;
    if (argc == 2)
        *(int *)argv[0] = rtc_renew_token_impl((const char *)argv[1]);
}

int mybot_agora_rtc_init(const char *app_id, const mybot_agora_rtc_callbacks_t *callbacks) {
    aosl_mpq_t q = (aosl_mpq_t)aosl_atomic_read(&s_rtc_mpq_id);
    if (q == AOSL_MPQ_INVALID) {
        q = aosl_mpq_create_flags(AOSL_MPQ_FLAG_SIGP_EVENT | AOSL_MPQ_FLAG_NONBLOCK,
                                  AOSL_THRD_PRI_NORMAL, 8192, 64, "mybot_rtc", NULL, NULL, NULL);
        if (q == AOSL_MPQ_INVALID)
            return -1;
        aosl_atomic_set(&s_rtc_mpq_id, q);
        aosl_atomic_set(&s_rtc_callbacks_enabled, true);
    }
    aosl_atomic_set(&s_rtc_callbacks_enabled, true);
    int result = -1;
    uintptr_t argv[] = {(uintptr_t)&result, (uintptr_t)app_id, (uintptr_t)callbacks};
    if (rtc_call("rtc_init", cmd_init, 3, argv) < 0)
        return -1;
    return result;
}

int mybot_agora_rtc_login_rtm(const char *uid, const char *token) {
    int result = -1;
    uintptr_t argv[] = {(uintptr_t)&result, (uintptr_t)uid, (uintptr_t)token};
    return rtc_call("rtc_login", cmd_login, 3, argv) < 0 ? -1 : result;
}
int mybot_agora_rtc_logout_rtm(void) {
    int result = -1;
    uintptr_t argv[] = {(uintptr_t)&result};
    return rtc_call("rtc_logout", cmd_logout, 1, argv) < 0 ? -1 : result;
}
bool mybot_agora_rtc_is_rtm_logged_in(void) {
    bool result = false;
    uintptr_t argv[] = {(uintptr_t)&result};
    (void)rtc_call("rtc_is_logged", cmd_is_logged, 1, argv);
    return result;
}
int mybot_agora_rtc_send_rtm_data(const char *peer, const void *data, size_t len, uint32_t id,
                                  const char *custom) {
    int result = -1;
    uintptr_t argv[] = {(uintptr_t)&result, (uintptr_t)peer, (uintptr_t)data,
                        (uintptr_t)len,     (uintptr_t)id,   (uintptr_t)custom};
    return rtc_call("rtc_send_rtm", cmd_send_rtm, 6, argv) < 0 ? -1 : result;
}
int mybot_agora_rtc_join(const char *channel, const char *token, const char *user) {
    int result = -1;
    uintptr_t argv[] = {(uintptr_t)&result, (uintptr_t)channel, (uintptr_t)token, (uintptr_t)user};
    return rtc_call("rtc_join", cmd_join, 4, argv) < 0 ? -1 : result;
}
int mybot_agora_rtc_leave(void) {
    int result = -1;
    uintptr_t argv[] = {(uintptr_t)&result};
    return rtc_call("rtc_leave", cmd_leave, 1, argv) < 0 ? -1 : result;
}
void mybot_agora_rtc_fini(void) {
    aosl_mpq_t q = (aosl_mpq_t)aosl_atomic_read(&s_rtc_mpq_id);
    if (q == AOSL_MPQ_INVALID)
        return;
    int result = -1;
    uintptr_t argv[] = {(uintptr_t)&result};
    if (rtc_call("rtc_fini", cmd_fini, 1, argv) < 0 || result < 0)
        return;
    (void)rtc_call("rtc_fini_barrier", cmd_barrier, 0, NULL);
    aosl_atomic_set(&s_rtc_mpq_id, AOSL_MPQ_INVALID);
    (void)aosl_mpq_destroy_wait(q);
}
int mybot_agora_rtc_send_audio(const void *data, size_t len) {
    int result = -1;
    uintptr_t argv[] = {(uintptr_t)&result, (uintptr_t)data, (uintptr_t)len};
    return rtc_call("rtc_send_audio", cmd_send_audio, 3, argv) < 0 ? -1 : result;
}
int mybot_agora_rtc_renew_token(const char *token) {
    int result = -1;
    uintptr_t argv[] = {(uintptr_t)&result, (uintptr_t)token};
    return rtc_call("rtc_renew", cmd_renew, 2, argv) < 0 ? -1 : result;
}
