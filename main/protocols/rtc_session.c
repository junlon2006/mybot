#include "rtc_session.h"
#include "mybot_config.h"

#include "agora_rtc_api.h"
#include <api/aosl_log.h>

#include <string.h>
#include <stdio.h>

#define TAG "RTC"

/* ---- internal state ---- */
static struct {
    mybot_rtc_state_t             state;
    mybot_rtc_session_callbacks_t cbs;
    connection_id_t               conn_id;
    bool                          initialized;
} s_rtc = {
    .state       = MYBOT_RTC_STATE_IDLE,
    .conn_id     = 0,
    .initialized = false,
};

static const char *state_str(mybot_rtc_state_t s)
{
    switch (s) {
    case MYBOT_RTC_STATE_IDLE:           return "IDLE";
    case MYBOT_RTC_STATE_INITIALIZED:    return "INITIALIZED";
    case MYBOT_RTC_STATE_CONNECTING:     return "CONNECTING";
    case MYBOT_RTC_STATE_CONNECTED:      return "CONNECTED";
    case MYBOT_RTC_STATE_RECONNECTING:   return "RECONNECTING";
    case MYBOT_RTC_STATE_DISCONNECTED:   return "DISCONNECTED";
    case MYBOT_RTC_STATE_ERROR:          return "ERROR";
    default:                             return "?";
    }
}

static void set_state(mybot_rtc_state_t st)
{
    if (s_rtc.state == st)
        return;
    s_rtc.state = st;
    AOSL_LOG_INF("[RTC] state -> %s", state_str(st));
    if (s_rtc.cbs.on_state_changed)
        s_rtc.cbs.on_state_changed(st);
}

/* ---- Agora SDK callbacks ---- */

static void __on_join_channel_success(connection_id_t conn_id, uint32_t uid, int elapsed)
{
    (void)conn_id;
    AOSL_LOG_INF("!!! join channel SUCCESS (uid=%u, elapsed=%d ms) !!!", uid, elapsed);
    set_state(MYBOT_RTC_STATE_CONNECTED);
}

static void __on_reconnecting(connection_id_t conn_id)
{
    (void)conn_id;
    AOSL_LOG_INF("[RTC] reconnecting...");
    set_state(MYBOT_RTC_STATE_RECONNECTING);
}

static void __on_connection_lost(connection_id_t conn_id)
{
    (void)conn_id;
    AOSL_LOG_INF("[RTC] connection lost");
    set_state(MYBOT_RTC_STATE_DISCONNECTED);
}

static void __on_rejoin_channel_success(connection_id_t conn_id, uint32_t uid, int elapsed_ms)
{
    (void)conn_id;
    (void)uid;
    (void)elapsed_ms;
    AOSL_LOG_INF("[RTC] rejoin channel success (uid=%u)", uid);
    set_state(MYBOT_RTC_STATE_CONNECTED);
}

static void __on_user_joined_with_user_account(connection_id_t conn_id, const user_info_t *user, int elapsed_ms)
{
    (void)conn_id;
    (void)elapsed_ms;
    AOSL_LOG_INF("[RTC] user \"%s\" (uid=%u) joined", user->user_account, user->uid);
}

static void __on_user_offline_with_user_account(connection_id_t conn_id, const user_info_t *user, int reason)
{
    (void)conn_id;
    (void)reason;
    AOSL_LOG_INF("[RTC] user \"%s\" (uid=%u) offline (reason=%d)",
                 user->user_account, user->uid, reason);
}

static void __on_audio_data(connection_id_t conn_id, const uint32_t uid, uint16_t sent_ts,
                            const void *data, size_t len, const audio_frame_info_t *info_ptr)
{
    (void)conn_id;
    (void)sent_ts;
    (void)info_ptr;
    if (s_rtc.cbs.on_remote_audio)
        s_rtc.cbs.on_remote_audio(uid, data, len);
}

static void __on_error(connection_id_t conn_id, int code, const char *msg)
{
    (void)conn_id;
    AOSL_LOG_ERR("[RTC] error (code=%d): %s", code, msg ? msg : "null");
    set_state(MYBOT_RTC_STATE_ERROR);
}

static void __on_license_failed(connection_id_t conn_id, int reason)
{
    (void)conn_id;
    (void)reason;
    AOSL_LOG_ERR("[RTC] license validation failed (reason=%d)", reason);
    set_state(MYBOT_RTC_STATE_ERROR);
}

static void __on_token_privilege_will_expire(connection_id_t conn_id, const char *token)
{
    (void)conn_id;
    (void)token;
    AOSL_LOG_INF("[RTC] token privilege will expire");
}

static void __on_rtc_stats(connection_id_t conn_id, rtc_stats_t stats)
{
    (void)conn_id;
    (void)stats;
    /* optional: log stats periodically */
}

/* ---- public API ---- */

int mybot_rtc_session_init(const char *app_id, mybot_rtc_session_callbacks_t *cbs)
{
    if (s_rtc.initialized)
        return 0;

    if (cbs)
        s_rtc.cbs = *cbs;

    /* Set up event handler */
    agora_rtc_event_handler_t handler;
    memset(&handler, 0, sizeof(handler));
    handler.on_join_channel_success           = __on_join_channel_success;
    handler.on_reconnecting                   = __on_reconnecting;
    handler.on_connection_lost                = __on_connection_lost;
    handler.on_rejoin_channel_success         = __on_rejoin_channel_success;
    handler.on_user_joined_with_user_account  = __on_user_joined_with_user_account;
    handler.on_user_offline_with_user_account = __on_user_offline_with_user_account;
    handler.on_audio_data                     = __on_audio_data;
    handler.on_error                          = __on_error;
    handler.on_license_validation_failure     = __on_license_failed;
    handler.on_token_privilege_will_expire    = __on_token_privilege_will_expire;
    handler.on_rtc_stats                      = __on_rtc_stats;

    rtc_service_option_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.area_code = AREA_CODE_GLOB;
    opt.log_cfg.log_level = RTC_LOG_INFO;
    opt.use_string_uid = true;
    snprintf(opt.license_value, sizeof(opt.license_value), "%s", "");

    AOSL_LOG_INF("calling agora_rtc_init(app_id=%s, use_string_uid=%d)",
                 app_id, opt.use_string_uid);

    int ret = agora_rtc_init((void *)app_id, &handler, &opt);
    if (ret < 0) {
        AOSL_LOG_ERR("agora_rtc_init failed: %s", agora_rtc_err_2_str(ret));
        return -1;
    }

    AOSL_LOG_INF("agora_rtc_init ok (sdk v%s)", agora_rtc_get_version());

    s_rtc.initialized = true;
    set_state(MYBOT_RTC_STATE_INITIALIZED);
    return 0;
}

int mybot_rtc_session_join(const char *channel, const char *token, const char *user_account)
{
    if (!s_rtc.initialized) {
        AOSL_LOG_ERR("[RTC] not initialized");
        return -1;
    }
    if (s_rtc.state == MYBOT_RTC_STATE_CONNECTED || s_rtc.state == MYBOT_RTC_STATE_CONNECTING) {
        AOSL_LOG_ERR("[RTC] already joining/joined");
        return -1;
    }

    /* Create connection */
    int ret = agora_rtc_create_connection(&s_rtc.conn_id);
    if (ret < 0) {
        AOSL_LOG_ERR("[RTC] create_connection failed: %s", agora_rtc_err_2_str(ret));
        return -1;
    }

    /* BWE parameters (defaults) */
    agora_rtc_set_bwe_param(s_rtc.conn_id, 16000, 256000, 64000);

    /* Channel options: PCM input → SDK encodes to G.722 */
    rtc_channel_options_t ch_opt;
    memset(&ch_opt, 0, sizeof(ch_opt));
    ch_opt.auto_subscribe_audio     = true;
    ch_opt.auto_subscribe_video     = false;
    ch_opt.enable_audio_jitter_buffer = true;
    ch_opt.enable_audio_mixer       = false;  /* per-user audio callback */
    ch_opt.enable_audio_decode      = true;
#if MYBOT_CLOUD_AEC
    ch_opt.enable_audio_downlink_aec = true;
#endif
#if MYBOT_AI_QOS
    ch_opt.enable_audio_ai_qos      = true;
#endif

    /* Tell SDK we'll send PCM; it will encode to G.722 */
    ch_opt.audio_codec_opt.audio_codec_type  = AUDIO_CODEC_TYPE_G722;
    ch_opt.audio_codec_opt.pcm_sample_rate   = 16000;
    ch_opt.audio_codec_opt.pcm_channel_num   = 1;
    ch_opt.audio_codec_opt.pcm_duration      = 20;  /* ms */

    const char *p_token = (token && token[0]) ? token : NULL;
    const char *p_user  = (user_account && user_account[0]) ? user_account : "default_user";

    AOSL_LOG_INF("joining channel: conn_id=%u, channel=%s, user=%s, has_token=%d",
                 s_rtc.conn_id, channel, p_user, p_token ? 1 : 0);
    AOSL_LOG_INF("  audio_codec=%d, pcm_rate=%d, pcm_chan=%d, pcm_duration=%d",
                 ch_opt.audio_codec_opt.audio_codec_type,
                 ch_opt.audio_codec_opt.pcm_sample_rate,
                 ch_opt.audio_codec_opt.pcm_channel_num,
                 ch_opt.audio_codec_opt.pcm_duration);

    set_state(MYBOT_RTC_STATE_CONNECTING);

    ret = agora_rtc_join_channel_with_user_account(s_rtc.conn_id, channel,
                                                    p_user, p_token, &ch_opt);
    if (ret < 0) {
        AOSL_LOG_ERR("join_channel failed: %s", agora_rtc_err_2_str(ret));
        agora_rtc_destroy_connection(s_rtc.conn_id);
        s_rtc.conn_id = 0;
        set_state(MYBOT_RTC_STATE_ERROR);
        return -1;
    }

    AOSL_LOG_INF("join_channel request sent, waiting for callback...");
    return 0;
}

int mybot_rtc_session_leave(void)
{
    if (!s_rtc.initialized || s_rtc.conn_id == 0)
        return 0;

    AOSL_LOG_INF("leaving channel (conn_id=%u)...", s_rtc.conn_id);

    int ret = agora_rtc_leave_channel(s_rtc.conn_id);
    if (ret < 0) {
        AOSL_LOG_ERR("leave_channel failed: %s", agora_rtc_err_2_str(ret));
    } else {
        AOSL_LOG_INF("leave_channel ok");
    }

    ret = agora_rtc_destroy_connection(s_rtc.conn_id);
    if (ret < 0) {
        AOSL_LOG_ERR("destroy_connection failed: %s", agora_rtc_err_2_str(ret));
    } else {
        AOSL_LOG_INF("connection destroyed");
    }

    s_rtc.conn_id = 0;
    set_state(MYBOT_RTC_STATE_INITIALIZED);
    return 0;
}

void mybot_rtc_session_fini(void)
{
    if (!s_rtc.initialized)
        return;

    if (s_rtc.conn_id != 0)
        mybot_rtc_session_leave();

    agora_rtc_fini();
    s_rtc.initialized = false;
    set_state(MYBOT_RTC_STATE_IDLE);
}

int mybot_rtc_session_send_audio(const void *data, size_t len)
{
    if (s_rtc.state != MYBOT_RTC_STATE_CONNECTED || s_rtc.conn_id == 0)
        return -1;

    audio_frame_info_t info;
    info.data_type = AUDIO_DATA_TYPE_PCM;

    int ret = agora_rtc_send_audio_data(s_rtc.conn_id, (void *)data, len, &info);
    if (ret < 0) {
        AOSL_LOG_ERR("[RTC] send_audio failed: %s", agora_rtc_err_2_str(ret));
        return -1;
    }
    return 0;
}

mybot_rtc_state_t mybot_rtc_session_get_state(void)
{
    return s_rtc.state;
}

bool mybot_rtc_session_is_connected(void)
{
    return s_rtc.state == MYBOT_RTC_STATE_CONNECTED;
}
