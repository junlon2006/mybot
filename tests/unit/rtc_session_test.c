/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_rtc_session.h"
#include <mybot/mybot_build_config.h>

#include "agora_rtc_api.h"
#include <api/aosl.h>
#include <hal/aosl_hal_time.h>

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define RTC_PCM_FRAME_SAMPLES (16000 * MYBOT_AUDIO_PTIME_MS / 1000)
#if MYBOT_CLOUD_AEC
#define RTC_PCM_FRAME_STREAMS 2
#else
#define RTC_PCM_FRAME_STREAMS 1
#endif
#define RTC_PCM_FRAME_BYTES (RTC_PCM_FRAME_SAMPLES * RTC_PCM_FRAME_STREAMS * sizeof(int16_t))

/* ---- Agora SDK stubs: capture the event handler and count calls. The real
 * x86_64 archive is not linked into this test; only the wrapper logic in
 * mybot_agora_rtc.c is exercised. ---- */
static agora_rtc_event_handler_t s_handler;
static int s_callback_owner;
static connection_id_t s_next_conn = 1;
static connection_id_t s_active_conn;
static int s_init_calls;
static int s_fini_calls;
static int s_join_calls;
static int s_leave_calls;
static int s_destroy_calls;
static int s_create_calls;
static int s_send_calls;
static int s_init_result;
static int s_create_result;
static int s_destroy_result;
static int s_join_result;
static int s_leave_result;
static int s_bwe_result;
static int s_send_result;
static int s_renew_calls;
static int s_renew_result;
static char s_renewed_token[512];
static int s_remote_audio_calls;
static int s_token_expiry_calls;
static int s_state_changes;
static mybot_rtc_state_t s_last_state;
static aosl_atomic_t s_block_next_state_callback;
static aosl_atomic_t s_state_callback_entered;
static aosl_atomic_t s_release_state_callback;
static aosl_atomic_t s_fini_reached;
static rtc_channel_options_t s_join_options;
static size_t s_last_send_len;
static audio_frame_info_t s_last_send_info;

const char *agora_rtc_get_version(void) {
    return "stub";
}

const char *agora_rtc_err_2_str(int err) {
    (void)err;
    return "stub-error";
}

int agora_rtc_init(const char *app_id, const agora_rtc_event_handler_t *event_handler,
                   rtc_service_option_t *opt) {
    (void)app_id;
    (void)opt;
    s_init_calls++;
    if (event_handler) {
        s_handler = *event_handler;
    }
    if (s_init_result < 0) {
        return s_init_result;
    }
    /* Mirror the real Agora SDK's independent AOSL ownership. */
    aosl_ctor();
    return s_init_result;
}

int agora_rtc_fini(void) {
    s_fini_calls++;
    aosl_atomic_set(&s_fini_reached, true);
    aosl_dtor();
    return 0;
}

int agora_rtc_create_connection(connection_id_t *conn_id) {
    s_create_calls++;
    if (s_create_result < 0) {
        return s_create_result;
    }
    *conn_id = s_next_conn++;
    s_active_conn = *conn_id;
    return s_create_result;
}

int agora_rtc_destroy_connection(connection_id_t conn_id) {
    (void)conn_id;
    s_destroy_calls++;
    return s_destroy_result;
}

int agora_rtc_join_channel_with_user_account(connection_id_t conn_id, const char *channel_name,
                                             const char *user_account, const char *token,
                                             rtc_channel_options_t *options) {
    (void)conn_id;
    (void)channel_name;
    (void)user_account;
    (void)token;
    assert(options != NULL);
    s_join_options = *options;
    s_join_calls++;
    return s_join_result;
}

int agora_rtc_leave_channel(connection_id_t conn_id) {
    (void)conn_id;
    s_leave_calls++;
    return s_leave_result;
}

int agora_rtc_renew_token(connection_id_t conn_id, const char *token) {
    assert(conn_id != 0);
    assert(token != NULL);
    snprintf(s_renewed_token, sizeof(s_renewed_token), "%s", token);
    s_renew_calls++;
    return s_renew_result;
}

int agora_rtc_set_bwe_param(connection_id_t conn_id, uint32_t min_bps, uint32_t max_bps,
                            uint32_t start_bps) {
    (void)conn_id;
    (void)min_bps;
    (void)max_bps;
    (void)start_bps;
    return s_bwe_result;
}

int agora_rtc_send_audio_data(connection_id_t conn_id, const void *data_ptr, size_t data_len,
                              audio_frame_info_t *info_ptr) {
    (void)conn_id;
    assert(data_ptr != NULL);
    assert(info_ptr != NULL);
    s_last_send_len = data_len;
    s_last_send_info = *info_ptr;
    s_send_calls++;
    return s_send_result;
}

/* ---- app callbacks ---- */
static void on_state_changed(mybot_rtc_state_t state, void *user_data) {
    assert(user_data == &s_callback_owner);
    s_last_state = state;
    s_state_changes++;
    if (aosl_atomic_xchg(&s_block_next_state_callback, false)) {
        aosl_atomic_set(&s_state_callback_entered, true);
        while (!aosl_atomic_read(&s_release_state_callback)) {
            aosl_hal_msleep(1);
        }
    }
}

static void on_remote_audio(uint32_t uid, const void *data, size_t len, void *user_data) {
    assert(user_data == &s_callback_owner);
    (void)uid;
    (void)data;
    (void)len;
    s_remote_audio_calls++;
}

static void on_token_will_expire(void *user_data) {
    assert(user_data == &s_callback_owner);
    s_token_expiry_calls++;
}

static void *fini_thread(void *arg) {
    mybot_rtc_session_fini(arg);
    return NULL;
}

static void *error_callback_thread(void *arg) {
    connection_id_t conn_id = *(const connection_id_t *)arg;
    s_handler.on_error(conn_id, -1, "async");
    return NULL;
}

int main(void) {
    aosl_ctor();

    mybot_rtc_session_t session = {0};
    unsigned char pcm_frame[RTC_PCM_FRAME_BYTES] = {0};

    assert(mybot_rtc_session_get_state(NULL) == MYBOT_RTC_STATE_IDLE);
    assert(!mybot_rtc_session_is_connected(NULL));
    assert(mybot_rtc_session_init(NULL, "app", NULL) < 0);
    assert(mybot_rtc_session_init(&session, NULL, NULL) < 0);
    assert(mybot_rtc_session_init(&session, "", NULL) < 0);

    s_init_result = -1;
    assert(mybot_rtc_session_init(&session, "test-app", NULL) < 0);
    assert(s_init_calls == 1);
    assert(session.lock == NULL);
    s_init_result = 0;

    /* Not initialized yet. */
    assert(mybot_rtc_session_get_state(&session) == MYBOT_RTC_STATE_IDLE);
    assert(!mybot_rtc_session_is_connected(&session));
    assert(mybot_rtc_session_join(&session, "room", "token", "user") < 0);
    assert(mybot_rtc_session_leave(&session) == 0);
    mybot_rtc_session_fini(&session); /* no agora_rtc_fini when uninitialized */
    assert(s_fini_calls == 0);
    assert(mybot_rtc_session_renew_token(&session, "renewed-token") < 0);
    assert(mybot_rtc_session_renew_token(&session, NULL) < 0);
    assert(mybot_rtc_session_renew_token(&session, "") < 0);
    assert(s_renew_calls == 0);

    mybot_rtc_session_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_state_changed = on_state_changed;
    cbs.on_remote_audio = on_remote_audio;
    cbs.on_token_will_expire = on_token_will_expire;
    cbs.user_data = &s_callback_owner;

    assert(mybot_rtc_session_init(&session, "test-app", &cbs) == 0);
    assert(s_init_calls == 2);
    assert(mybot_rtc_session_get_state(&session) == MYBOT_RTC_STATE_INITIALIZED);

    mybot_rtc_session_t other_session = {0};
    assert(mybot_rtc_session_init(&other_session, "other-app", &cbs) < 0);

    /* Double init is a no-op. */
    assert(mybot_rtc_session_init(&session, "test-app", &cbs) == 0);
    assert(s_init_calls == 2);

    /* Cannot send before joining. */
    assert(mybot_rtc_session_send_audio(&session, pcm_frame, sizeof(pcm_frame)) < 0);
    assert(s_send_calls == 0);

    s_create_result = -1;
    assert(mybot_rtc_session_join(&session, "room", "token", "user1") < 0);
    assert(s_create_calls == 1);
    s_create_result = 0;

    s_join_result = -1;
    assert(mybot_rtc_session_join(&session, "room", "token", "user1") < 0);
    assert(mybot_rtc_session_get_state(&session) == MYBOT_RTC_STATE_ERROR);
    assert(s_destroy_calls == 1);
    s_join_result = 0;
    s_bwe_result = -1;

    /* Join: connecting until the SDK reports success. */
    assert(mybot_rtc_session_join(&session, "room", "token", "user1") == 0);
    s_bwe_result = 0;
    assert(s_join_calls == 2);
    assert(s_create_calls == 3);
    assert(s_join_options.audio_codec_opt.audio_codec_type == AUDIO_CODEC_TYPE_G722);
    assert(s_join_options.audio_codec_opt.pcm_sample_rate == 16000);
    assert(s_join_options.audio_codec_opt.pcm_channel_num == 1);
    assert(s_join_options.audio_codec_opt.pcm_duration == MYBOT_AUDIO_PTIME_MS);
    assert(s_join_options.audio_jitter_frame_duration == MYBOT_AUDIO_PTIME_MS);
    assert(s_join_options.enable_audio_downlink_aec == (MYBOT_CLOUD_AEC != 0));
    assert(s_join_options.enable_audio_ai_qos == (MYBOT_AI_QOS != 0));
    assert(mybot_rtc_session_get_state(&session) == MYBOT_RTC_STATE_CONNECTING);
    assert(!mybot_rtc_session_is_connected(&session));

    /* A callback from a different connection must not affect this session. */
    s_handler.on_connection_lost(s_active_conn + 100);
    assert(mybot_rtc_session_get_state(&session) == MYBOT_RTC_STATE_CONNECTING);

    s_handler.on_join_channel_success(s_active_conn, 42, 100);
    assert(mybot_rtc_session_is_connected(&session));
    assert(mybot_rtc_session_get_state(&session) == MYBOT_RTC_STATE_CONNECTED);
    s_handler.on_reconnecting(s_active_conn);
    assert(mybot_rtc_session_get_state(&session) == MYBOT_RTC_STATE_RECONNECTING);
    s_handler.on_rejoin_channel_success(s_active_conn, 42, 10);
    assert(mybot_rtc_session_is_connected(&session));

    s_handler.on_token_privilege_will_expire(s_active_conn, "old-token");
    assert(s_token_expiry_calls == 1);
    assert(mybot_rtc_session_renew_token(&session, "renewed-token") == 0);
    assert(s_renew_calls == 1);
    assert(strcmp(s_renewed_token, "renewed-token") == 0);
    s_renew_result = -1;
    assert(mybot_rtc_session_renew_token(&session, "rejected-token") < 0);
    assert(s_renew_calls == 2);
    s_renew_result = 0;

    s_send_result = -1;
    assert(mybot_rtc_session_send_audio(&session, pcm_frame, sizeof(pcm_frame)) < 0);
    s_send_result = 0;
    assert(mybot_rtc_session_send_audio(&session, pcm_frame, sizeof(pcm_frame)) == 0);
    assert(s_last_send_len == sizeof(pcm_frame));
    assert(s_last_send_info.data_type == AUDIO_DATA_TYPE_PCM);
    assert(s_send_calls == 2);

    /* Connection loss blocks sends until the rejoin callback. */
    s_handler.on_connection_lost(s_active_conn);
    assert(mybot_rtc_session_get_state(&session) == MYBOT_RTC_STATE_DISCONNECTED);
    assert(mybot_rtc_session_send_audio(&session, pcm_frame, sizeof(pcm_frame)) < 0);
    s_handler.on_rejoin_channel_success(s_active_conn, 42, 50);
    assert(mybot_rtc_session_is_connected(&session));
    assert(mybot_rtc_session_send_audio(&session, pcm_frame, sizeof(pcm_frame)) == 0);
    assert(s_send_calls == 3);
    assert(s_last_send_len == sizeof(pcm_frame));

    /* License failure moves to ERROR. */
    s_handler.on_license_validation_failure(s_active_conn, 2);
    assert(mybot_rtc_session_get_state(&session) == MYBOT_RTC_STATE_ERROR);
    assert(!mybot_rtc_session_is_connected(&session));

    /* Recover to CONNECTED, then a second join is rejected while joined. */
    s_handler.on_join_channel_success(s_active_conn, 42, 0);
    assert(mybot_rtc_session_is_connected(&session));
    assert(mybot_rtc_session_join(&session, "room2", "token", "user2") < 0);

    user_info_t user = {0};
    user.uid = 9;
    snprintf(user.user_account, sizeof(user.user_account), "%s", "remote-user");
    s_handler.on_user_joined_with_user_account(s_active_conn, &user, 1);
    s_handler.on_user_offline_with_user_account(s_active_conn, &user, 2);
    rtc_stats_t stats = {0};
    s_handler.on_rtc_stats(s_active_conn, stats);
    s_handler.on_error(s_active_conn, -1, NULL);
    assert(mybot_rtc_session_get_state(&session) == MYBOT_RTC_STATE_ERROR);
    s_handler.on_join_channel_success(s_active_conn, 42, 0);

    /* Remote audio forwards to the application callback. */
    s_handler.on_audio_data(s_active_conn, 7, 0, "audio", 5, NULL);
    assert(s_remote_audio_calls == 1);

    /* Leave tears down the connection and returns to INITIALIZED. */
    s_leave_result = -1;
    s_destroy_result = -1;
    assert(mybot_rtc_session_leave(&session) == 0);
    s_leave_result = 0;
    s_destroy_result = 0;
    assert(s_leave_calls == 1);
    assert(s_destroy_calls == 2);
    assert(mybot_rtc_session_get_state(&session) == MYBOT_RTC_STATE_INITIALIZED);
    assert(mybot_rtc_session_leave(&session) == 0); /* idempotent without a connection */
    assert(s_leave_calls == 1);
    assert(mybot_rtc_session_renew_token(&session, "renewed-token") < 0);
    assert(s_renew_calls == 2);

    /* Fini detaches the bridge and waits for a callback already in flight. */
    assert(mybot_rtc_session_join(&session, "room", "token", "user") == 0);
    s_handler.on_join_channel_success(s_active_conn, 42, 0);
    assert(mybot_rtc_session_is_connected(&session));

    aosl_atomic_set(&s_block_next_state_callback, true);
    aosl_atomic_set(&s_state_callback_entered, false);
    aosl_atomic_set(&s_release_state_callback, false);
    aosl_atomic_set(&s_fini_reached, false);

    pthread_t callback_thread;
    pthread_t teardown_thread;
    assert(pthread_create(&callback_thread, NULL, error_callback_thread, &s_active_conn) == 0);
    while (!aosl_atomic_read(&s_state_callback_entered)) {
        aosl_hal_msleep(1);
    }
    assert(pthread_create(&teardown_thread, NULL, fini_thread, &session) == 0);
    while (!aosl_atomic_read(&s_fini_reached)) {
        aosl_hal_msleep(1);
    }
    /* callback_bridge_wait() has not released the session mutex yet. */
    assert(session.lock != NULL);
    aosl_atomic_set(&s_release_state_callback, true);
    assert(pthread_join(callback_thread, NULL) == 0);
    assert(pthread_join(teardown_thread, NULL) == 0);

    /* Finalize calls agora_rtc_fini exactly once. */
    assert(s_fini_calls == 1);
    assert(!mybot_rtc_session_is_connected(&session));
    assert(mybot_rtc_session_join(&session, "room", "token", "user") < 0);
    assert(mybot_rtc_session_leave(&session) == 0);
    mybot_rtc_session_fini(&session); /* now uninitialized again */
    assert(s_fini_calls == 1);
    assert(s_state_changes > 0);

    int state_changes_after_fini = s_state_changes;
    int remote_audio_after_fini = s_remote_audio_calls;
    s_handler.on_join_channel_success(s_active_conn, 42, 0);
    s_handler.on_reconnecting(s_active_conn);
    s_handler.on_connection_lost(s_active_conn);
    s_handler.on_rejoin_channel_success(s_active_conn, 42, 0);
    s_handler.on_audio_data(s_active_conn, 7, 0, "audio", 5, NULL);
    s_handler.on_error(s_active_conn, 1, "late");
    s_handler.on_license_validation_failure(s_active_conn, 1);
    s_handler.on_token_privilege_will_expire(s_active_conn, "late");
    assert(s_state_changes == state_changes_after_fini);
    assert(s_remote_audio_calls == remote_audio_after_fini);

    aosl_dtor();
    puts("rtc_session_test: ok");
    return 0;
}
