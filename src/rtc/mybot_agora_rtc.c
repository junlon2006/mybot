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

typedef struct {
    mybot_rtc_state_t state;
    connection_id_t conn_id;
    mybot_agora_rtc_callbacks_t callbacks;
    char app_id[64];
    bool fini_failed;
} mybot_agora_rtc_t;

static mybot_agora_rtc_t s_rtc = {.conn_id = CONNECTION_ID_INVALID};
static aosl_atomic_t s_rtc_lifecycle_gate;

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

static void on_rtc_stats(connection_id_t conn_id, rtc_stats_t stats) {
    (void)conn_id;
    (void)stats;
}

static void clear_runtime_state(void) {
    s_rtc.conn_id = CONNECTION_ID_INVALID;
    memset(&s_rtc.callbacks, 0, sizeof(s_rtc.callbacks));
    s_rtc.app_id[0] = '\0';
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
    handler.on_rtc_stats = on_rtc_stats;

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

int mybot_agora_rtc_join(const char *channel, const char *token, const char *user_account) {
    if (!channel || !channel[0] || !user_account || !user_account[0]) {
        AOSL_LOG_ERR("[RTC] join rejected: invalid channel or user account");
        return -1;
    }
    rtc_lock();

    int ret = -1;
    if (s_rtc.state != MYBOT_RTC_STATE_INITIALIZED || s_rtc.conn_id != CONNECTION_ID_INVALID) {
        AOSL_LOG_ERR("[RTC] join rejected (state=%s, conn_id=%u)", state_name(s_rtc.state),
                     s_rtc.conn_id);
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
    channel_options.audio_jitter_frame_duration = MYBOT_AUDIO_PTIME_MS;
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
    return ret;
}

int mybot_agora_rtc_leave(void) {
    rtc_lock();

    connection_id_t conn_id = s_rtc.conn_id;
    if (conn_id == CONNECTION_ID_INVALID) {
        rtc_unlock();
        return 0;
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
    return destroy_ret < 0 ? -1 : 0;
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
