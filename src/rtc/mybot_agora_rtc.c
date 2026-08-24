/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_rtc_session.h"
#include <mybot/mybot_build_config.h>

#include "agora_rtc_api.h"
#include <api/aosl_log.h>
#include <api/aosl_atomic.h>
#include <api/aosl_thread.h>
#include <hal/aosl_hal_thread.h>
#include <hal/aosl_hal_time.h>

#include <string.h>
#include <stdio.h>

#define TAG "RTC"

/* Agora initializes one process-wide service and does not attach user data to
 * callbacks. The bridge serializes callback admission and teardown so an
 * in-flight callback keeps the caller-owned session alive until it returns. */
static aosl_static_lock_t s_callback_bridge_lock = AOSL_STATIC_LOCK_INIT;
static mybot_rtc_session_t *s_active_session;

static int callback_bridge_lock(void) {
    return aosl_static_lock_lock(&s_callback_bridge_lock);
}

static void callback_bridge_unlock(void) {
    (void)aosl_static_lock_unlock(&s_callback_bridge_lock);
}

static mybot_rtc_session_t *callback_enter(connection_id_t conn_id) {
    mybot_rtc_session_t *session = NULL;

    if (conn_id == 0 || callback_bridge_lock() < 0) {
        return NULL;
    }

    if (s_active_session && !s_active_session->callback_closing &&
        s_active_session->callback_conn_id == conn_id) {
        session = s_active_session;
        session->callback_count++;
    }
    callback_bridge_unlock();
    return session;
}

static void callback_exit(mybot_rtc_session_t *session) {
    if (!session || callback_bridge_lock() < 0) {
        return;
    }
    if (session->callback_count > 0) {
        session->callback_count--;
    }
    callback_bridge_unlock();
}

static int callback_bridge_wait(mybot_rtc_session_t *session) {
    for (;;) {
        unsigned int count;
        if (callback_bridge_lock() < 0) {
            return -1;
        }
        count = session->callback_count;
        callback_bridge_unlock();
        if (count == 0) {
            return 0;
        }
        aosl_hal_msleep(1);
    }
}

static const char *state_str(mybot_rtc_state_t s) {
    switch (s) {
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
    default:
        return "?";
    }
}

static void set_state(mybot_rtc_session_t *session, mybot_rtc_state_t st) {
    if ((mybot_rtc_state_t)aosl_atomic_read(&session->state) == st) {
        return;
    }
    aosl_atomic_set(&session->state, (intptr_t)st);
    AOSL_LOG_NTC("[RTC] state -> %s", state_str(st));
    if (session->cbs.on_state_changed) {
        session->cbs.on_state_changed(st, session->cbs.user_data);
    }
}

/* ---- Agora SDK callbacks ---- */

static void __on_join_channel_success(connection_id_t conn_id, uint32_t uid, int elapsed) {
    mybot_rtc_session_t *session = callback_enter(conn_id);
    if (!session) {
        return;
    }
    AOSL_LOG_NTC("!!! join channel SUCCESS (uid=%u, elapsed=%d ms) !!!", uid, elapsed);
    set_state(session, MYBOT_RTC_STATE_CONNECTED);
    callback_exit(session);
}

static void __on_reconnecting(connection_id_t conn_id) {
    mybot_rtc_session_t *session = callback_enter(conn_id);
    if (!session) {
        return;
    }
    AOSL_LOG_NTC("[RTC] reconnecting...");
    set_state(session, MYBOT_RTC_STATE_RECONNECTING);
    callback_exit(session);
}

static void __on_connection_lost(connection_id_t conn_id) {
    mybot_rtc_session_t *session = callback_enter(conn_id);
    if (!session) {
        return;
    }
    AOSL_LOG_NTC("[RTC] connection lost");
    set_state(session, MYBOT_RTC_STATE_DISCONNECTED);
    callback_exit(session);
}

static void __on_rejoin_channel_success(connection_id_t conn_id, uint32_t uid, int elapsed_ms) {
    mybot_rtc_session_t *session = callback_enter(conn_id);
    if (!session) {
        return;
    }
    (void)uid;
    (void)elapsed_ms;
    AOSL_LOG_NTC("[RTC] rejoin channel success (uid=%u)", uid);
    set_state(session, MYBOT_RTC_STATE_CONNECTED);
    callback_exit(session);
}

static void __on_user_joined_with_user_account(connection_id_t conn_id, const user_info_t *user,
                                               int elapsed_ms) {
    mybot_rtc_session_t *session = callback_enter(conn_id);
    if (!session) {
        return;
    }
    (void)elapsed_ms;
    AOSL_LOG_NTC("[RTC] user \"%s\" (uid=%u) joined", user->user_account, user->uid);
    callback_exit(session);
}

static void __on_user_offline_with_user_account(connection_id_t conn_id, const user_info_t *user,
                                                int reason) {
    mybot_rtc_session_t *session = callback_enter(conn_id);
    if (!session) {
        return;
    }
    AOSL_LOG_NTC("[RTC] user \"%s\" (uid=%u) offline (reason=%d)", user->user_account, user->uid,
                 reason);
    callback_exit(session);
}

static void __on_audio_data(connection_id_t conn_id, const uint32_t uid, uint16_t sent_ts,
                            const void *data, size_t len, const audio_frame_info_t *info_ptr) {
    mybot_rtc_session_t *session = callback_enter(conn_id);
    if (!session) {
        return;
    }
    (void)sent_ts;
    (void)info_ptr;
    if (session->cbs.on_remote_audio) {
        session->cbs.on_remote_audio(uid, data, len, session->cbs.user_data);
    }
    callback_exit(session);
}

static void __on_error(connection_id_t conn_id, int code, const char *msg) {
    mybot_rtc_session_t *session = callback_enter(conn_id);
    if (!session) {
        return;
    }
    AOSL_LOG_ERR("[RTC] error (code=%d): %s", code, msg ? msg : "null");
    set_state(session, MYBOT_RTC_STATE_ERROR);
    callback_exit(session);
}

static void __on_license_failed(connection_id_t conn_id, int reason) {
    mybot_rtc_session_t *session = callback_enter(conn_id);
    if (!session) {
        return;
    }
    AOSL_LOG_ERR("[RTC] license validation failed (reason=%d)", reason);
    set_state(session, MYBOT_RTC_STATE_ERROR);
    callback_exit(session);
}

static void __on_token_privilege_will_expire(connection_id_t conn_id, const char *token) {
    mybot_rtc_session_t *session = callback_enter(conn_id);
    if (!session) {
        return;
    }
    (void)token;
    AOSL_LOG_NTC("[RTC] token privilege will expire");
    if (session->cbs.on_token_will_expire) {
        session->cbs.on_token_will_expire(session->cbs.user_data);
    }
    callback_exit(session);
}

static void __on_rtc_stats(connection_id_t conn_id, rtc_stats_t stats) {
    (void)conn_id;
    (void)stats;
    /* Optional hook for periodic statistics logging. */
}

/* ---- Session API ---- */

int mybot_rtc_session_init(mybot_rtc_session_t *session, const char *app_id,
                           const mybot_rtc_session_callbacks_t *cbs) {
    if (!session || !app_id || !app_id[0]) {
        return -1;
    }

    if (callback_bridge_lock() < 0) {
        return -1;
    }
    if (s_active_session && s_active_session != session) {
        callback_bridge_unlock();
        AOSL_LOG_ERR("another RTC session is already active");
        return -1;
    }
    if (aosl_atomic_read(&session->initialized)) {
        callback_bridge_unlock();
        return 0;
    }
    session->callback_conn_id = 0;
    session->callback_count = 0;
    session->callback_closing = false;
    s_active_session = session;
    callback_bridge_unlock();

    if (!session->lock) {
        session->lock = aosl_hal_mutex_create();
        if (!session->lock) {
            AOSL_LOG_ERR("rtc lock create failed");
            if (callback_bridge_lock() == 0) {
                if (s_active_session == session) {
                    s_active_session = NULL;
                }
                session->callback_closing = true;
                callback_bridge_unlock();
            }
            return -1;
        }
    }

    if (cbs) {
        session->cbs = *cbs;
    }

    /* Set up event handler */
    agora_rtc_event_handler_t handler;
    memset(&handler, 0, sizeof(handler));
    handler.on_join_channel_success = __on_join_channel_success;
    handler.on_reconnecting = __on_reconnecting;
    handler.on_connection_lost = __on_connection_lost;
    handler.on_rejoin_channel_success = __on_rejoin_channel_success;
    handler.on_user_joined_with_user_account = __on_user_joined_with_user_account;
    handler.on_user_offline_with_user_account = __on_user_offline_with_user_account;
    handler.on_audio_data = __on_audio_data;
    handler.on_error = __on_error;
    handler.on_license_validation_failure = __on_license_failed;
    handler.on_token_privilege_will_expire = __on_token_privilege_will_expire;
    handler.on_rtc_stats = __on_rtc_stats;

    rtc_service_option_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.area_code = AREA_CODE_GLOB;
    opt.log_cfg.log_level = RTC_LOG_NOTICE;
    opt.use_string_uid = true;
    snprintf(opt.license_value, sizeof(opt.license_value), "%s", "");

    AOSL_LOG_NTC("calling agora_rtc_init(app_id=%s, use_string_uid=%d)", app_id,
                 opt.use_string_uid);

    int ret = agora_rtc_init((void *)app_id, &handler, &opt);
    if (ret < 0) {
        AOSL_LOG_ERR("agora_rtc_init failed: %s", agora_rtc_err_2_str(ret));
        aosl_hal_mutex_destroy(session->lock);
        session->lock = NULL;
        if (callback_bridge_lock() == 0) {
            if (s_active_session == session) {
                s_active_session = NULL;
            }
            session->callback_closing = true;
            callback_bridge_unlock();
        }
        return -1;
    }

    AOSL_LOG_NTC("agora_rtc_init ok (sdk v%s)", agora_rtc_get_version());

    aosl_atomic_set(&session->initialized, true);
    set_state(session, MYBOT_RTC_STATE_INITIALIZED);
    return 0;
}

int mybot_rtc_session_join(mybot_rtc_session_t *session, const char *channel, const char *token,
                           const char *user_account) {
    if (!session || !aosl_atomic_read(&session->initialized)) {
        AOSL_LOG_ERR("[RTC] not initialized");
        return -1;
    }

    int ret = 0;

    aosl_hal_mutex_lock(session->lock);

    mybot_rtc_state_t cur = (mybot_rtc_state_t)aosl_atomic_read(&session->state);
    if (cur == MYBOT_RTC_STATE_CONNECTED || cur == MYBOT_RTC_STATE_CONNECTING) {
        AOSL_LOG_ERR("[RTC] already joining/joined");
        ret = -1;
        goto out;
    }

    /* Create connection */
    ret = agora_rtc_create_connection(&session->conn_id);
    if (ret < 0) {
        AOSL_LOG_ERR("[RTC] create_connection failed: %s", agora_rtc_err_2_str(ret));
        goto out;
    }

    if (callback_bridge_lock() < 0) {
        agora_rtc_destroy_connection(session->conn_id);
        session->conn_id = 0;
        ret = -1;
        goto out;
    }
    session->callback_conn_id = session->conn_id;
    callback_bridge_unlock();

    /* BWE parameters (defaults) */
    int bwe_ret = agora_rtc_set_bwe_param(session->conn_id, 16000, 256000, 64000);
    if (bwe_ret < 0) {
        AOSL_LOG_WRN("[RTC] set_bwe_param failed: %s", agora_rtc_err_2_str(bwe_ret));
    }

    /* Channel options: PCM input → SDK encodes to G.722 */
    rtc_channel_options_t ch_opt = {0};
    ch_opt.auto_subscribe_audio = true;
    ch_opt.auto_subscribe_video = false;
    ch_opt.enable_audio_jitter_buffer = true;
    ch_opt.audio_jitter_frame_duration = MYBOT_AUDIO_PTIME_MS;
    ch_opt.enable_audio_mixer = false; /* per-user audio callback */
    ch_opt.enable_audio_decode = true;
#if MYBOT_CLOUD_AEC
    ch_opt.enable_audio_downlink_aec = true;
#endif
#if MYBOT_AI_QOS
    ch_opt.enable_audio_ai_qos = true;
#endif

    /* Tell SDK we'll send PCM; it will encode to G.722 */
    ch_opt.audio_codec_opt.audio_codec_type = AUDIO_CODEC_TYPE_G722;
    ch_opt.audio_codec_opt.pcm_sample_rate = 16000;
    /* Cloud AEC uses the service's paired [mic, ref] payload convention. The
     * pair is interpreted out of band and is not SDK stereo. */
    ch_opt.audio_codec_opt.pcm_channel_num = 1;
    ch_opt.audio_codec_opt.pcm_duration = MYBOT_AUDIO_PTIME_MS;

    const char *p_token = (token && token[0]) ? token : NULL;
    const char *p_user = (user_account && user_account[0]) ? user_account : "default_user";

    AOSL_LOG_NTC("joining channel: conn_id=%u, channel=%s, user=%s, has_token=%d", session->conn_id,
                 channel, p_user, p_token ? 1 : 0);
    AOSL_LOG_NTC("audio_codec=%d, pcm_rate=%d, pcm_chan=%d, pcm_duration=%d, "
                 "jitter_frame_duration=%d",
                 ch_opt.audio_codec_opt.audio_codec_type, ch_opt.audio_codec_opt.pcm_sample_rate,
                 ch_opt.audio_codec_opt.pcm_channel_num, ch_opt.audio_codec_opt.pcm_duration,
                 ch_opt.audio_jitter_frame_duration);

    set_state(session, MYBOT_RTC_STATE_CONNECTING);

    ret = agora_rtc_join_channel_with_user_account(session->conn_id, channel, p_user, p_token,
                                                   &ch_opt);
    if (ret < 0) {
        AOSL_LOG_ERR("join_channel failed: %s", agora_rtc_err_2_str(ret));
        if (callback_bridge_lock() == 0) {
            session->callback_conn_id = 0;
            callback_bridge_unlock();
        }
        agora_rtc_destroy_connection(session->conn_id);
        session->conn_id = 0;
        set_state(session, MYBOT_RTC_STATE_ERROR);
        goto out;
    }

    AOSL_LOG_NTC("join_channel request sent, waiting for callback...");

out:
    aosl_hal_mutex_unlock(session->lock);
    return ret;
}

int mybot_rtc_session_leave(mybot_rtc_session_t *session) {
    if (!session) {
        return 0;
    }

    /* initialized is published only after the mutex is created. conn_id is
     * deliberately checked only while holding that mutex. */
    if (!aosl_atomic_read(&session->initialized)) {
        return 0;
    }

    aosl_hal_mutex_lock(session->lock);

    if (!aosl_atomic_read(&session->initialized) || session->conn_id == 0) {
        aosl_hal_mutex_unlock(session->lock);
        return 0;
    }

    AOSL_LOG_NTC("leaving channel (conn_id=%u)...", session->conn_id);

    if (callback_bridge_lock() == 0) {
        session->callback_conn_id = 0;
        callback_bridge_unlock();
    }

    int ret = agora_rtc_leave_channel(session->conn_id);
    if (ret < 0) {
        AOSL_LOG_ERR("leave_channel failed: %s", agora_rtc_err_2_str(ret));
    } else {
        AOSL_LOG_NTC("leave_channel ok");
    }

    ret = agora_rtc_destroy_connection(session->conn_id);
    if (ret < 0) {
        AOSL_LOG_ERR("destroy_connection failed: %s", agora_rtc_err_2_str(ret));
    } else {
        AOSL_LOG_NTC("connection destroyed");
    }

    session->conn_id = 0;
    aosl_hal_mutex_unlock(session->lock);

    set_state(session, MYBOT_RTC_STATE_INITIALIZED);
    return 0;
}

void mybot_rtc_session_fini(mybot_rtc_session_t *session) {
    if (!session || !aosl_atomic_read(&session->initialized)) {
        return;
    }

    /* RTSA owns a process-wide callback queue. Detach this session before
     * tearing down the connection so new callbacks are rejected. RTSA's
     * agora_rtc_fini() waits its callback queue; callback_bridge_wait() then
     * covers callbacks that had already passed admission before the detach. */
    if (callback_bridge_lock() < 0) {
        AOSL_LOG_ERR("RTC callback bridge lock failed during fini");
        return;
    }
    if (s_active_session == session) {
        s_active_session = NULL;
    }
    session->callback_closing = true;
    session->callback_conn_id = 0;
    callback_bridge_unlock();

    /* leave() is idempotent and checks conn_id while holding the mutex. */
    mybot_rtc_session_leave(session);

    /* agora_rtc_fini() destroys the SDK queues and releases the SDK's own
     * AOSL reference. The application reference remains held by mybot_start()
     * until mybot_stop() completes all application teardown. */
    aosl_atomic_set(&session->initialized, false);
    set_state(session, MYBOT_RTC_STATE_IDLE);

    agora_rtc_fini();
    if (callback_bridge_wait(session) < 0) {
        AOSL_LOG_ERR("RTC callback bridge wait failed during fini");
        return;
    }
    memset(&session->cbs, 0, sizeof(session->cbs));

    if (session->lock) {
        aosl_hal_mutex_destroy(session->lock);
        session->lock = NULL;
    }
}

int mybot_rtc_session_send_audio(mybot_rtc_session_t *session, const void *data, size_t len) {
    int ret;

    if (!session || !aosl_atomic_read(&session->initialized)) {
        return -1;
    }

    aosl_hal_mutex_lock(session->lock);

    if ((mybot_rtc_state_t)aosl_atomic_read(&session->state) != MYBOT_RTC_STATE_CONNECTED ||
        session->conn_id == 0) {
        ret = -1;
        goto out;
    }

    audio_frame_info_t info;
    info.data_type = AUDIO_DATA_TYPE_PCM;

    ret = agora_rtc_send_audio_data(session->conn_id, (void *)data, len, &info);
    if (ret < 0) {
        AOSL_LOG_ERR("[RTC] send_audio failed: %s", agora_rtc_err_2_str(ret));
        goto out;
    }

out:
    aosl_hal_mutex_unlock(session->lock);
    return ret;
}

int mybot_rtc_session_renew_token(mybot_rtc_session_t *session, const char *token) {
    if (!session || !token || !token[0] || !aosl_atomic_read(&session->initialized)) {
        return -1;
    }

    aosl_hal_mutex_lock(session->lock);

    int ret = -1;
    if (aosl_atomic_read(&session->initialized) && session->conn_id != 0) {
        ret = agora_rtc_renew_token(session->conn_id, token);
        if (ret < 0) {
            AOSL_LOG_ERR("[RTC] renew_token failed: %s", agora_rtc_err_2_str(ret));
        } else {
            AOSL_LOG_NTC("[RTC] token renewed");
        }
    }

    aosl_hal_mutex_unlock(session->lock);
    return ret;
}

mybot_rtc_state_t mybot_rtc_session_get_state(const mybot_rtc_session_t *session) {
    if (!session) {
        return MYBOT_RTC_STATE_IDLE;
    }
    return (mybot_rtc_state_t)aosl_atomic_read(&session->state);
}

bool mybot_rtc_session_is_connected(const mybot_rtc_session_t *session) {
    if (!session) {
        return false;
    }
    return (mybot_rtc_state_t)aosl_atomic_read(&session->state) == MYBOT_RTC_STATE_CONNECTED;
}
