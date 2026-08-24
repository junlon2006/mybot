/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_agora_rtc.h"

#include <mybot/mybot_build_config.h>

#include "agora_rtc_api.h"
#include <api/aosl.h>
#include <api/aosl_atomic.h>
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

static agora_rtc_event_handler_t s_handler;
static connection_id_t s_next_conn = 1;
static connection_id_t s_last_conn;
static int s_init_result;
static int s_create_result;
static int s_join_result;
static int s_leave_result;
static int s_destroy_result;
static int s_send_result;
static int s_renew_result;
static int s_bwe_result;
static int s_init_calls;
static int s_fini_calls;
static int s_create_calls;
static int s_join_calls;
static int s_leave_calls;
static int s_destroy_calls;
static int s_send_calls;
static int s_renew_calls;
static int s_operation_seq;
static int s_leave_seq;
static int s_destroy_seq;
static int s_fini_seq;
static int s_remote_audio_calls;
static int s_token_expiry_calls;
static int s_state_calls;
static mybot_rtc_state_t s_last_state;
static rtc_channel_options_t s_join_options;
static char s_renewed_token[512];
static size_t s_last_send_len;
static audio_frame_info_t s_last_send_info;
static int s_callback_owner;
static aosl_atomic_t s_block_send;
static aosl_atomic_t s_send_entered;
static aosl_atomic_t s_release_send;
static aosl_atomic_t s_block_state_callback;
static aosl_atomic_t s_state_callback_entered;
static aosl_atomic_t s_release_state_callback;
static aosl_atomic_t s_leave_started;
static aosl_atomic_t s_leave_returned;
static aosl_atomic_t s_probe_fini_callback;
static aosl_atomic_t s_fini_callback_returned;

const char *agora_rtc_get_version(void) {
    return "stub";
}

const char *agora_rtc_err_2_str(int error) {
    (void)error;
    return "stub-error";
}

int agora_rtc_init(const char *app_id, const agora_rtc_event_handler_t *handler,
                   rtc_service_option_t *options) {
    assert(app_id != NULL);
    assert(options != NULL);
    s_init_calls++;
    if (handler) {
        s_handler = *handler;
    }
    if (s_init_result < 0) {
        return s_init_result;
    }
    aosl_ctor();
    return 0;
}

static void *fini_callback_thread(void *arg) {
    connection_id_t conn_id = *(const connection_id_t *)arg;
    s_handler.on_join_channel_success(conn_id, 42, 0);
    aosl_atomic_set(&s_fini_callback_returned, true);
    return NULL;
}

int agora_rtc_fini(void) {
    s_fini_calls++;
    s_fini_seq = ++s_operation_seq;
    if (aosl_atomic_xchg(&s_probe_fini_callback, false)) {
        aosl_atomic_set(&s_fini_callback_returned, false);
        pthread_t callback_thread;
        int thread_ret = pthread_create(&callback_thread, NULL, fini_callback_thread, &s_last_conn);
        assert(thread_ret == 0);
        for (int elapsed = 0; elapsed < 1000; elapsed++) {
            if (aosl_atomic_read(&s_fini_callback_returned)) {
                break;
            }
            aosl_hal_msleep(1);
        }
        assert(aosl_atomic_read(&s_fini_callback_returned));
        thread_ret = pthread_join(callback_thread, NULL);
        assert(thread_ret == 0);
    }
    aosl_dtor();
    return 0;
}

int agora_rtc_create_connection(connection_id_t *conn_id) {
    s_create_calls++;
    if (s_create_result < 0) {
        return s_create_result;
    }
    *conn_id = s_next_conn++;
    s_last_conn = *conn_id;
    return 0;
}

int agora_rtc_destroy_connection(connection_id_t conn_id) {
    assert(conn_id != CONNECTION_ID_INVALID);
    s_destroy_calls++;
    s_destroy_seq = ++s_operation_seq;
    return s_destroy_result;
}

int agora_rtc_join_channel_with_user_account(connection_id_t conn_id, const char *channel,
                                             const char *user_account, const char *token,
                                             rtc_channel_options_t *options) {
    assert(conn_id == s_last_conn);
    assert(channel != NULL);
    assert(user_account != NULL);
    assert(options != NULL);
    (void)token;
    s_join_calls++;
    s_join_options = *options;
    return s_join_result;
}

int agora_rtc_leave_channel(connection_id_t conn_id) {
    assert(conn_id != CONNECTION_ID_INVALID);
    s_leave_calls++;
    s_leave_seq = ++s_operation_seq;
    return s_leave_result;
}

int agora_rtc_set_bwe_param(connection_id_t conn_id, uint32_t min_bps, uint32_t max_bps,
                            uint32_t start_bps) {
    assert(conn_id != CONNECTION_ID_INVALID);
    (void)min_bps;
    (void)max_bps;
    (void)start_bps;
    return s_bwe_result;
}

int agora_rtc_send_audio_data(connection_id_t conn_id, const void *data, size_t len,
                              audio_frame_info_t *info) {
    assert(conn_id != CONNECTION_ID_INVALID);
    assert(data != NULL);
    assert(info != NULL);
    aosl_atomic_set(&s_send_entered, true);
    while (aosl_atomic_read(&s_block_send) && !aosl_atomic_read(&s_release_send)) {
        aosl_hal_msleep(1);
    }
    s_send_calls++;
    s_last_send_len = len;
    s_last_send_info = *info;
    return s_send_result;
}

int agora_rtc_renew_token(connection_id_t conn_id, const char *token) {
    assert(conn_id != CONNECTION_ID_INVALID);
    assert(token != NULL);
    s_renew_calls++;
    snprintf(s_renewed_token, sizeof(s_renewed_token), "%s", token);
    return s_renew_result;
}

static void on_state_changed(mybot_rtc_state_t state, void *user_data) {
    assert(user_data == &s_callback_owner);
    s_state_calls++;
    s_last_state = state;
    if (aosl_atomic_xchg(&s_block_state_callback, false)) {
        aosl_atomic_set(&s_state_callback_entered, true);
        while (!aosl_atomic_read(&s_release_state_callback)) {
            aosl_hal_msleep(1);
        }
    }
}

static void on_remote_audio(uint32_t uid, const void *data, size_t len, void *user_data) {
    assert(user_data == &s_callback_owner);
    assert(uid == 7);
    assert(data != NULL);
    assert(len == 5);
    s_remote_audio_calls++;
}

static void on_token_will_expire(void *user_data) {
    assert(user_data == &s_callback_owner);
    s_token_expiry_calls++;
}

static bool wait_for_atomic(const aosl_atomic_t *value, intptr_t expected, int timeout_ms) {
    for (int elapsed = 0; elapsed < timeout_ms; elapsed++) {
        if (aosl_atomic_read(value) == expected) {
            return true;
        }
        aosl_hal_msleep(1);
    }
    return false;
}

static void *send_thread(void *arg) {
    size_t len = *(const size_t *)arg;
    static unsigned char frame[RTC_PCM_FRAME_BYTES];
    int ret = mybot_agora_rtc_send_audio(frame, len);
    return (void *)(intptr_t)ret;
}

static void *leave_thread(void *arg) {
    (void)arg;
    aosl_atomic_set(&s_leave_started, true);
    int ret = mybot_agora_rtc_leave();
    aosl_atomic_set(&s_leave_returned, true);
    return (void *)(intptr_t)ret;
}

static void *error_callback_thread(void *arg) {
    connection_id_t conn_id = *(const connection_id_t *)arg;
    s_handler.on_error(conn_id, -1, "async");
    return NULL;
}

int main(void) {
    aosl_ctor();

    unsigned char pcm_frame[RTC_PCM_FRAME_BYTES] = {0};
    char oversized_app_id[65];
    memset(oversized_app_id, 'a', sizeof(oversized_app_id) - 1);
    oversized_app_id[sizeof(oversized_app_id) - 1] = '\0';
    mybot_agora_rtc_callbacks_t callbacks = {
        .on_remote_audio = on_remote_audio,
        .on_token_will_expire = on_token_will_expire,
        .on_state_changed = on_state_changed,
        .user_data = &s_callback_owner,
    };

    assert(mybot_agora_rtc_init(NULL, NULL) < 0);
    assert(mybot_agora_rtc_init("", NULL) < 0);
    assert(mybot_agora_rtc_init(oversized_app_id, NULL) < 0);
    assert(s_init_calls == 0);
    assert(mybot_agora_rtc_join("room", "token", "user") < 0);
    assert(mybot_agora_rtc_leave() == 0);
    assert(mybot_agora_rtc_send_audio(NULL, sizeof(pcm_frame)) < 0);
    assert(mybot_agora_rtc_send_audio(pcm_frame, sizeof(pcm_frame)) < 0);
    assert(mybot_agora_rtc_renew_token(NULL) < 0);
    assert(mybot_agora_rtc_renew_token("") < 0);
    assert(mybot_agora_rtc_renew_token("token") < 0);

    assert(mybot_agora_rtc_init("app-1", &callbacks) == 0);
    assert(s_init_calls == 1);
    assert(mybot_agora_rtc_init("app-1", NULL) == 0);
    assert(mybot_agora_rtc_init("app-1", &callbacks) == 0);
    assert(s_init_calls == 1);
    assert(mybot_agora_rtc_init("app-2", &callbacks) < 0);
    assert(s_init_calls == 1);

    assert(mybot_agora_rtc_join(NULL, "token", "user") < 0);
    assert(mybot_agora_rtc_join("", "token", "user") < 0);
    assert(mybot_agora_rtc_join("room", "token", NULL) < 0);

    s_create_result = -1;
    assert(mybot_agora_rtc_join("room", "token", "user") < 0);
    s_create_result = 0;

    s_join_result = -1;
    assert(mybot_agora_rtc_join("room", "token", "user") < 0);
    assert(s_destroy_calls == 1);
    s_join_result = 0;

    s_bwe_result = -1;
    assert(mybot_agora_rtc_join("room", "token", "user") == 0);
    s_bwe_result = 0;
    connection_id_t first_conn = s_last_conn;
    assert(mybot_agora_rtc_init("app-1", &callbacks) < 0);
    assert(mybot_agora_rtc_join("room-duplicate", "token", "user") < 0);
    assert(s_join_options.audio_codec_opt.audio_codec_type == AUDIO_CODEC_TYPE_G722);
    assert(s_join_options.audio_codec_opt.pcm_sample_rate == 16000);
    assert(s_join_options.audio_codec_opt.pcm_channel_num == 1);
    assert(s_join_options.audio_codec_opt.pcm_duration == MYBOT_AUDIO_PTIME_MS);
    assert(s_join_options.enable_audio_downlink_aec == (MYBOT_CLOUD_AEC != 0));
    assert(s_join_options.enable_audio_ai_qos == (MYBOT_AI_QOS != 0));

    int states_before_wrong_conn = s_state_calls;
    s_handler.on_join_channel_success(first_conn + 100, 42, 0);
    assert(s_state_calls == states_before_wrong_conn);
    s_handler.on_join_channel_success(first_conn, 42, 10);
    assert(s_last_state == MYBOT_RTC_STATE_CONNECTED);
    s_handler.on_reconnecting(first_conn);
    assert(s_last_state == MYBOT_RTC_STATE_RECONNECTING);
    s_handler.on_connection_lost(first_conn);
    assert(s_last_state == MYBOT_RTC_STATE_DISCONNECTED);
    assert(mybot_agora_rtc_send_audio(pcm_frame, sizeof(pcm_frame)) < 0);
    s_handler.on_rejoin_channel_success(first_conn, 42, 10);
    assert(s_last_state == MYBOT_RTC_STATE_CONNECTED);

    user_info_t user = {0};
    user.uid = 7;
    snprintf(user.user_account, sizeof(user.user_account), "%s", "remote-user");
    s_handler.on_user_joined_with_user_account(first_conn, NULL, 0);
    s_handler.on_user_joined_with_user_account(first_conn, &user, 0);
    s_handler.on_user_offline_with_user_account(first_conn, NULL, 0);
    s_handler.on_user_offline_with_user_account(first_conn, &user, 0);
    rtc_stats_t stats = {0};
    s_handler.on_rtc_stats(first_conn, stats);

    s_handler.on_error(first_conn, -1, NULL);
    assert(s_last_state == MYBOT_RTC_STATE_ERROR);
    s_handler.on_join_channel_success(first_conn, 42, 0);
    int states_before_global_error = s_state_calls;
    s_handler.on_error(CONNECTION_ID_ALL, -1, "global");
    assert(s_state_calls == states_before_global_error);
    s_handler.on_license_validation_failure(first_conn, 1);
    assert(s_last_state == MYBOT_RTC_STATE_ERROR);
    s_handler.on_join_channel_success(first_conn, 42, 0);
    s_handler.on_token_privilege_will_expire(first_conn, "old-token");
    s_handler.on_audio_data(first_conn, 7, 0, "audio", 5, NULL);
    assert(s_token_expiry_calls == 1);
    assert(s_remote_audio_calls == 1);

    assert(mybot_agora_rtc_send_audio(pcm_frame, 0) < 0);
    s_send_result = -1;
    assert(mybot_agora_rtc_send_audio(pcm_frame, sizeof(pcm_frame)) < 0);
    s_send_result = 0;
    assert(mybot_agora_rtc_send_audio(pcm_frame, sizeof(pcm_frame)) == 0);
    assert(s_send_calls == 2);
    assert(s_last_send_len == sizeof(pcm_frame));
    assert(s_last_send_info.data_type == AUDIO_DATA_TYPE_PCM);
    s_renew_result = -1;
    assert(mybot_agora_rtc_renew_token("rejected-token") < 0);
    s_renew_result = 0;
    assert(mybot_agora_rtc_renew_token("renewed-token") == 0);
    assert(strcmp(s_renewed_token, "renewed-token") == 0);

    aosl_atomic_set(&s_block_send, true);
    aosl_atomic_set(&s_send_entered, false);
    aosl_atomic_set(&s_release_send, false);
    aosl_atomic_set(&s_leave_started, false);
    aosl_atomic_set(&s_leave_returned, false);
    size_t frame_len = sizeof(pcm_frame);
    pthread_t sender;
    pthread_t leaver;
    int thread_ret = pthread_create(&sender, NULL, send_thread, &frame_len);
    assert(thread_ret == 0);
    assert(wait_for_atomic(&s_send_entered, true, 1000));
    int destroys_before_leave = s_destroy_calls;
    s_leave_result = -1;
    thread_ret = pthread_create(&leaver, NULL, leave_thread, NULL);
    assert(thread_ret == 0);
    assert(wait_for_atomic(&s_leave_started, true, 1000));
    aosl_hal_msleep(50);
    assert(!aosl_atomic_read(&s_leave_returned));
    assert(s_destroy_calls == destroys_before_leave);
    aosl_atomic_set(&s_release_send, true);
    void *thread_result = NULL;
    assert(pthread_join(sender, &thread_result) == 0);
    assert((intptr_t)thread_result == 0);
    assert(pthread_join(leaver, &thread_result) == 0);
    assert((intptr_t)thread_result == 0);
    assert(s_leave_seq < s_destroy_seq);
    s_leave_result = 0;
    aosl_atomic_set(&s_block_send, false);
    assert(mybot_agora_rtc_leave() == 0);
    assert(mybot_agora_rtc_renew_token("renewed-token") < 0);

    int state_after_leave = s_state_calls;
    int audio_after_leave = s_remote_audio_calls;
    s_handler.on_join_channel_success(first_conn, 42, 0);
    s_handler.on_audio_data(first_conn, 7, 0, "audio", 5, NULL);
    assert(s_state_calls == state_after_leave);
    assert(s_remote_audio_calls == audio_after_leave);

    assert(mybot_agora_rtc_init("app-1", &callbacks) == 0);
    assert(s_init_calls == 1);
    assert(mybot_agora_rtc_join("room-2", "token", "user") == 0);
    connection_id_t second_conn = s_last_conn;
    assert(second_conn != first_conn);
    s_handler.on_join_channel_success(second_conn, 42, 0);

    aosl_atomic_set(&s_block_state_callback, true);
    aosl_atomic_set(&s_state_callback_entered, false);
    aosl_atomic_set(&s_release_state_callback, false);
    aosl_atomic_set(&s_leave_started, false);
    aosl_atomic_set(&s_leave_returned, false);
    pthread_t callback_thread;
    thread_ret = pthread_create(&callback_thread, NULL, error_callback_thread, &second_conn);
    assert(thread_ret == 0);
    assert(wait_for_atomic(&s_state_callback_entered, true, 1000));
    destroys_before_leave = s_destroy_calls;
    thread_ret = pthread_create(&leaver, NULL, leave_thread, NULL);
    assert(thread_ret == 0);
    assert(wait_for_atomic(&s_leave_started, true, 1000));
    aosl_hal_msleep(50);
    assert(!aosl_atomic_read(&s_leave_returned));
    assert(s_destroy_calls == destroys_before_leave);
    aosl_atomic_set(&s_release_state_callback, true);
    assert(pthread_join(callback_thread, NULL) == 0);
    assert(pthread_join(leaver, &thread_result) == 0);
    assert((intptr_t)thread_result == 0);

    assert(mybot_agora_rtc_init("app-1", &callbacks) == 0);
    assert(mybot_agora_rtc_join("active-at-fini", "token", "user") == 0);
    connection_id_t final_conn = s_last_conn;
    s_handler.on_join_channel_success(final_conn, 42, 0);
    int leaves_before_fini = s_leave_calls;
    int destroys_before_fini = s_destroy_calls;
    aosl_atomic_set(&s_probe_fini_callback, true);
    mybot_agora_rtc_fini();
    assert(s_fini_calls == 1);
    assert(s_leave_calls == leaves_before_fini + 1);
    assert(s_destroy_calls == destroys_before_fini + 1);
    assert(s_leave_seq < s_destroy_seq);
    assert(s_destroy_seq < s_fini_seq);
    states_before_wrong_conn = s_state_calls;
    s_handler.on_join_channel_success(final_conn, 42, 0);
    assert(s_state_calls == states_before_wrong_conn);
    mybot_agora_rtc_fini();
    assert(s_fini_calls == 1);

    assert(mybot_agora_rtc_init("app-1", &callbacks) == 0);
    assert(s_init_calls == 2);
    assert(mybot_agora_rtc_join("destroy-failure", "token", "user") == 0);
    s_handler.on_join_channel_success(s_last_conn, 42, 0);
    s_destroy_result = -1;
    assert(mybot_agora_rtc_leave() < 0);
    assert(s_last_state == MYBOT_RTC_STATE_ERROR);
    assert(mybot_agora_rtc_join("blocked", "token", "user") < 0);
    assert(mybot_agora_rtc_init("app-1", &callbacks) < 0);
    mybot_agora_rtc_fini();
    assert(s_fini_calls == 2);

    assert(mybot_agora_rtc_init("app-1", &callbacks) == 0);
    assert(s_init_calls == 3);
    s_join_result = -1;
    assert(mybot_agora_rtc_join("join-cleanup-failure", "token", "user") < 0);
    assert(s_last_state == MYBOT_RTC_STATE_ERROR);
    assert(mybot_agora_rtc_join("blocked", "token", "user") < 0);
    s_join_result = 0;
    s_destroy_result = 0;
    mybot_agora_rtc_fini();
    assert(s_fini_calls == 3);

    s_init_result = -1;
    assert(mybot_agora_rtc_init("app-fail", &callbacks) < 0);
    assert(s_init_calls == 4);
    s_init_result = 0;
    assert(mybot_agora_rtc_init("app-fail", &callbacks) < 0);
    assert(s_init_calls == 4);
    mybot_agora_rtc_fini();
    assert(s_fini_calls == 3);

    aosl_dtor();
    puts("agora_rtc_test: ok");
    return 0;
}
