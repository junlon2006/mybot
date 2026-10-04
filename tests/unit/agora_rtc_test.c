/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_agora_rtc.h"

#include <mybot/mybot_build_config.h>

#include "agora_rtc_api.h"
#include <api/aosl.h>
#include <api/aosl_atomic.h>
#include <api/aosl_log.h>
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
#define RTC_REMOTE_PCM_FRAME_BYTES (RTC_PCM_FRAME_SAMPLES * sizeof(int16_t))

#if MYBOT_ENABLE_VIDEO
#define TEST_VIDEO_MIN_BPS 32000U
#define TEST_VIDEO_MAX_BPS 256000U
#else
#define TEST_VIDEO_MIN_BPS 0U
#define TEST_VIDEO_MAX_BPS 0U
#endif

#define mybot_agora_rtc_join(channel, token, user)                                                 \
    mybot_agora_rtc_join(channel, token, user, TEST_VIDEO_MIN_BPS, TEST_VIDEO_MAX_BPS)

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
static uint32_t s_last_bwe_min_bps;
static uint32_t s_last_bwe_max_bps;
static uint32_t s_last_bwe_start_bps;
static int s_rtm_login_result;
static int s_rtm_logout_result;
static int s_rtm_send_result;
static int s_rtm_subscribe_result;
static int s_rtm_unsubscribe_result;
static bool s_auto_rtm_login_event;
static rtm_err_code_e s_auto_rtm_login_error;
static bool s_auto_rtm_subscribe_event;
static rtm_err_code_e s_auto_rtm_subscribe_error;
static pthread_t s_rtm_login_thread;
static bool s_rtm_login_thread_valid;
static pthread_t s_rtm_subscribe_thread;
static bool s_rtm_subscribe_thread_valid;
static int s_init_calls;
static int s_fini_calls;
static int s_create_calls;
static int s_join_calls;
static int s_leave_calls;
static int s_destroy_calls;
static int s_send_calls;
static int s_renew_calls;
static int s_rtm_login_calls;
static int s_rtm_logout_calls;
static int s_rtm_send_calls;
static int s_rtm_subscribe_calls;
static int s_rtm_unsubscribe_calls;
static int s_operation_seq;
static int s_leave_seq;
static int s_destroy_seq;
static int s_fini_seq;
static int s_remote_audio_calls;
static int s_token_expiry_calls;
static int s_state_calls;
static int s_rtm_event_calls;
static int s_rtm_data_calls;
static int s_rtm_send_result_calls;
static int s_rtm_subscribe_result_calls;
static int s_rtm_subscribe_data_calls;
static mybot_rtc_state_t s_last_state;
static mybot_rtm_event_type_t s_last_rtm_event;
static int s_last_rtm_error;
static mybot_rtm_message_state_t s_last_rtm_message_state;
static char s_last_rtm_event_uid[AGORA_RTM_UID_MAX_LEN];
static char s_last_rtm_data_uid[AGORA_RTM_UID_MAX_LEN];
static char s_last_rtm_send_uid[AGORA_RTM_UID_MAX_LEN];
static uint32_t s_last_rtm_result_msg_id;
static size_t s_last_rtm_data_len;
static char s_last_rtm_custom_type[64];
static unsigned char s_last_rtm_data[AGORA_RTM_DATA_MAX_LEN];
static unsigned char s_last_rtm_subscribe_data[AGORA_RTM_DATA_MAX_LEN];
static unsigned char s_last_remote_audio[RTC_REMOTE_PCM_FRAME_BYTES];
static char s_last_rtm_subscribe_channel[AGORA_RTC_CHANNEL_NAME_MAX_LEN + 1];
static char s_last_rtm_subscribe_uid[AGORA_RTM_UID_MAX_LEN];
static size_t s_last_rtm_subscribe_data_len;
static int s_last_rtm_subscribe_error;
static rtc_channel_options_t s_join_options;
static char s_renewed_token[512];
static char s_rtm_uid[AGORA_RTM_UID_MAX_LEN];
static char s_rtm_token[512];
static char s_rtm_peer_uid[AGORA_RTM_UID_MAX_LEN];
static char s_rtm_custom_type[64];
static uint32_t s_rtm_msg_id;
static size_t s_rtm_msg_len;
static rtm_message_type_e s_rtm_msg_type;
static char s_rtm_channel[AGORA_RTC_CHANNEL_NAME_MAX_LEN + 1];
static agora_rtm_handler_t s_rtm_handler;
static size_t s_last_send_len;
static audio_frame_info_t s_last_send_info;
static int s_callback_owner;
#if MYBOT_ENABLE_VIDEO
static int s_video_key_frame_requests;
static uint32_t s_video_target_bitrate;
static video_frame_info_t s_last_video_info;
#endif
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
static aosl_atomic_t s_rtm_data_delivered;

#ifdef MYBOT_TEST_WRAP_RTC_ALLOC
typedef struct {
    unsigned calls;
    unsigned fail_at;
    bool failed;
} callback_allocation_fault_t;

static pthread_key_t s_allocation_fault_key;
static bool s_allocation_fault_key_ready;

void *__real_aosl_hal_malloc(size_t size);

void *__wrap_aosl_hal_malloc(size_t size) {
    callback_allocation_fault_t *fault =
        s_allocation_fault_key_ready ? pthread_getspecific(s_allocation_fault_key) : NULL;
    if (fault && ++fault->calls == fault->fail_at) {
        fault->failed = true;
        return NULL;
    }
    return __real_aosl_hal_malloc(size);
}
#endif

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
    assert(options->log_cfg.log_level == RTC_LOG_ERROR);
    /* Model RTSA's process-wide AOSL logging side effect, including failure. */
    aosl_set_log_level((int)options->log_cfg.log_level);
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
    s_last_bwe_min_bps = min_bps;
    s_last_bwe_max_bps = max_bps;
    s_last_bwe_start_bps = start_bps;
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

#if MYBOT_ENABLE_VIDEO
int agora_rtc_send_video_data(connection_id_t conn_id, const void *data, size_t len,
                              video_frame_info_t *info) {
    assert(conn_id != CONNECTION_ID_INVALID);
    assert(data != NULL);
    assert(len > 0);
    assert(info != NULL);
    s_send_calls++;
    s_last_send_len = len;
    s_last_video_info = *info;
    return s_send_result;
}
#endif

int agora_rtc_renew_token(connection_id_t conn_id, const char *token) {
    assert(conn_id != CONNECTION_ID_INVALID);
    assert(token != NULL);
    s_renew_calls++;
    snprintf(s_renewed_token, sizeof(s_renewed_token), "%s", token);
    return s_renew_result;
}

static void *rtm_login_event_thread(void *arg) {
    (void)arg;
    aosl_hal_msleep(1);
    s_rtm_handler.on_rtm_event(s_rtm_uid, RTM_EVENT_TYPE_LOGIN, s_auto_rtm_login_error);
    return NULL;
}

static void join_rtm_login_thread(void) {
    if (s_rtm_login_thread_valid) {
        assert(pthread_join(s_rtm_login_thread, NULL) == 0);
        s_rtm_login_thread_valid = false;
    }
}

int agora_rtm_login(const char *rtm_uid, const char *rtm_token,
                    const agora_rtm_handler_t *handler) {
    assert(rtm_uid != NULL);
    assert(rtm_uid[0] != '\0');
    join_rtm_login_thread();
    s_rtm_login_calls++;
    snprintf(s_rtm_uid, sizeof(s_rtm_uid), "%s", rtm_uid);
    snprintf(s_rtm_token, sizeof(s_rtm_token), "%s", rtm_token ? rtm_token : "");
    if (handler) {
        s_rtm_handler = *handler;
    } else {
        memset(&s_rtm_handler, 0, sizeof(s_rtm_handler));
    }
    if (s_rtm_login_result >= 0 && s_auto_rtm_login_event) {
        assert(pthread_create(&s_rtm_login_thread, NULL, rtm_login_event_thread, NULL) == 0);
        s_rtm_login_thread_valid = true;
    }
    return s_rtm_login_result;
}

int agora_rtm_logout(void) {
    s_rtm_logout_calls++;
    return s_rtm_logout_result;
}

int agora_rtm_send_data(const char *rtm_uid, const void *msg, size_t msg_len, uint32_t msg_id,
                        rtm_message_type_e msg_type, const char *custom_type) {
    assert(rtm_uid != NULL);
    assert(rtm_uid[0] != '\0');
    assert(msg != NULL || msg_len == 0);
    s_rtm_send_calls++;
    snprintf(s_rtm_peer_uid, sizeof(s_rtm_peer_uid), "%s", rtm_uid);
    s_rtm_msg_len = msg_len;
    s_rtm_msg_id = msg_id;
    s_rtm_msg_type = msg_type;
    snprintf(s_rtm_custom_type, sizeof(s_rtm_custom_type), "%s", custom_type ? custom_type : "");
    return s_rtm_send_result;
}

static void *rtm_subscribe_event_thread(void *arg) {
    (void)arg;
    aosl_hal_msleep(1);
    s_rtm_handler.on_rtm_subscribe_result(s_rtm_channel, s_auto_rtm_subscribe_error);
    return NULL;
}

static void join_rtm_subscribe_thread(void) {
    if (s_rtm_subscribe_thread_valid) {
        assert(pthread_join(s_rtm_subscribe_thread, NULL) == 0);
        s_rtm_subscribe_thread_valid = false;
    }
}

int agora_rtm_subscribe(const char *channel_name) {
    assert(channel_name != NULL);
    assert(channel_name[0] != '\0');
    join_rtm_subscribe_thread();
    s_rtm_subscribe_calls++;
    snprintf(s_rtm_channel, sizeof(s_rtm_channel), "%s", channel_name);
    if (s_rtm_subscribe_result >= 0 && s_auto_rtm_subscribe_event) {
        assert(pthread_create(&s_rtm_subscribe_thread, NULL, rtm_subscribe_event_thread, NULL) ==
               0);
        s_rtm_subscribe_thread_valid = true;
    }
    return s_rtm_subscribe_result;
}

int agora_rtm_unsubscribe(const char *channel_name) {
    assert(channel_name != NULL);
    assert(strcmp(channel_name, s_rtm_channel) == 0);
    s_rtm_unsubscribe_calls++;
    return s_rtm_unsubscribe_result;
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
    assert(len == RTC_REMOTE_PCM_FRAME_BYTES);
    memcpy(s_last_remote_audio, data, len);
    s_remote_audio_calls++;
}

static void on_token_will_expire(void *user_data) {
    assert(user_data == &s_callback_owner);
    s_token_expiry_calls++;
}

#if MYBOT_ENABLE_VIDEO
static void on_video_key_frame_requested(void *user_data) {
    assert(user_data == &s_callback_owner);
    s_video_key_frame_requests++;
}

static void on_video_target_bitrate_changed(uint32_t target_bps, void *user_data) {
    assert(user_data == &s_callback_owner);
    s_video_target_bitrate = target_bps;
}
#endif

static void on_rtm_event(const char *rtm_uid, mybot_rtm_event_type_t event_type, int error_code,
                         void *user_data) {
    assert(user_data == &s_callback_owner);
    assert(rtm_uid != NULL);
    s_rtm_event_calls++;
    s_last_rtm_event = event_type;
    s_last_rtm_error = error_code;
    snprintf(s_last_rtm_event_uid, sizeof(s_last_rtm_event_uid), "%s", rtm_uid);
}

static void on_rtm_data(const char *rtm_uid, const void *data, size_t len, const char *custom_type,
                        void *user_data) {
    assert(user_data == &s_callback_owner);
    assert(rtm_uid != NULL);
    assert(data != NULL || len == 0);
    s_rtm_data_calls++;
    s_last_rtm_data_len = len;
    assert(len <= sizeof(s_last_rtm_data));
    if (len) {
        memcpy(s_last_rtm_data, data, len);
    }
    snprintf(s_last_rtm_data_uid, sizeof(s_last_rtm_data_uid), "%s", rtm_uid);
    snprintf(s_last_rtm_custom_type, sizeof(s_last_rtm_custom_type), "%s",
             custom_type ? custom_type : "");
    aosl_atomic_inc(&s_rtm_data_delivered);
}

static void on_rtm_send_data_result(const char *rtm_uid, uint32_t msg_id,
                                    mybot_rtm_message_state_t state, void *user_data) {
    assert(user_data == &s_callback_owner);
    assert(rtm_uid != NULL);
    s_rtm_send_result_calls++;
    s_last_rtm_result_msg_id = msg_id;
    s_last_rtm_message_state = state;
    snprintf(s_last_rtm_send_uid, sizeof(s_last_rtm_send_uid), "%s", rtm_uid);
}

static void on_rtm_subscribe_result(const char *channel, int error_code, void *user_data) {
    assert(user_data == &s_callback_owner);
    assert(channel != NULL);
    s_rtm_subscribe_result_calls++;
    s_last_rtm_subscribe_error = error_code;
    snprintf(s_last_rtm_subscribe_channel, sizeof(s_last_rtm_subscribe_channel), "%s", channel);
}

static void on_rtm_subscribe_data(const char *channel, const char *rtm_uid, const void *data,
                                  size_t len, const char *custom_type, void *user_data) {
    assert(user_data == &s_callback_owner);
    assert(channel != NULL);
    assert(rtm_uid != NULL);
    assert(data != NULL || len == 0);
    s_rtm_subscribe_data_calls++;
    s_last_rtm_subscribe_data_len = len;
    assert(len <= sizeof(s_last_rtm_subscribe_data));
    if (len) {
        memcpy(s_last_rtm_subscribe_data, data, len);
    }
    snprintf(s_last_rtm_subscribe_channel, sizeof(s_last_rtm_subscribe_channel), "%s", channel);
    snprintf(s_last_rtm_subscribe_uid, sizeof(s_last_rtm_subscribe_uid), "%s", rtm_uid);
    snprintf(s_last_rtm_custom_type, sizeof(s_last_rtm_custom_type), "%s",
             custom_type ? custom_type : "");
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

typedef enum { PAYLOAD_RTM, PAYLOAD_CHANNEL, PAYLOAD_PCM } callback_payload_t;

static void synchronize_rtc(void) {
    /* The synchronous MPQ query runs after already queued vendor events and
     * application callbacks, so their ordinary observation fields are safe
     * to read after it returns. Producers must finish posting before this. */
    assert(mybot_agora_rtc_is_rtm_logged_in());
}

static void block_payload_owner(connection_id_t conn) {
    s_handler.on_join_channel_success(conn, 42, 0);
    synchronize_rtc();
    aosl_atomic_set(&s_block_state_callback, true);
    aosl_atomic_set(&s_state_callback_entered, false);
    aosl_atomic_set(&s_release_state_callback, false);
    s_handler.on_reconnecting(conn);
    assert(wait_for_atomic(&s_state_callback_entered, true, 1000));
    /* Restore CONNECTED on the owner before it delivers the queued payloads. */
    s_handler.on_rejoin_channel_success(conn, 42, 0);
}

static void post_borrowed_payload(callback_payload_t type, connection_id_t conn, char *uid,
                                  char *channel, char *custom, void *data, size_t len,
                                  const audio_frame_info_t *info) {
    switch (type) {
    case PAYLOAD_RTM:
        s_rtm_handler.on_rtm_data(uid, data, len, RTM_MESSAGE_TYPE_BINARY, custom);
        break;
    case PAYLOAD_CHANNEL:
        s_rtm_handler.on_rtm_subscribe_data(channel, uid, data, len, RTM_MESSAGE_TYPE_STRING,
                                            custom);
        break;
    case PAYLOAD_PCM:
        s_handler.on_audio_data(conn, 7, 0, data, len, info);
        break;
    }
}

static int payload_callback_count(callback_payload_t type) {
    return type == PAYLOAD_RTM       ? s_rtm_data_calls
           : type == PAYLOAD_CHANNEL ? s_rtm_subscribe_data_calls
                                     : s_remote_audio_calls;
}

static void expect_copied_payload(callback_payload_t type, const void *expected, size_t len) {
    if (type == PAYLOAD_PCM) {
        assert(memcmp(s_last_remote_audio, expected, len) == 0);
        return;
    }
    if (type == PAYLOAD_RTM) {
        assert(s_last_rtm_data_len == len);
        assert(memcmp(s_last_rtm_data, expected, len) == 0);
        assert(strcmp(s_last_rtm_data_uid, "original-peer") == 0);
    } else {
        assert(s_last_rtm_subscribe_data_len == len);
        assert(memcmp(s_last_rtm_subscribe_data, expected, len) == 0);
        assert(strcmp(s_last_rtm_subscribe_channel, "payload-room") == 0);
        assert(strcmp(s_last_rtm_subscribe_uid, "original-peer") == 0);
    }
    assert(strcmp(s_last_rtm_custom_type, "original-type") == 0);
}

static void test_callback_payload_ownership(const mybot_agora_rtc_callbacks_t *callbacks) {
    assert(mybot_agora_rtc_init("payload-app", callbacks) == 0);
    assert(mybot_agora_rtc_join("payload-room", "token", "user") == 0);
    connection_id_t conn = s_last_conn;
    join_rtm_login_thread();
    join_rtm_subscribe_thread();
    static unsigned char expected[RTC_REMOTE_PCM_FRAME_BYTES];
    static unsigned char payload[RTC_REMOTE_PCM_FRAME_BYTES];
    for (size_t i = 0; i < sizeof(expected); ++i) {
        expected[i] = (unsigned char)(i * 17U + 3U);
    }
    for (int type = PAYLOAD_RTM; type <= PAYLOAD_PCM; ++type) {
        size_t len = type == PAYLOAD_PCM ? sizeof(payload) : 16;
        char uid[] = "original-peer";
        char channel[] = "payload-room";
        char custom[] = "original-type";
        audio_frame_info_t info = {.data_type = AUDIO_DATA_TYPE_PCM};
        memcpy(payload, expected, len);
        block_payload_owner(conn);
        int callbacks_before = payload_callback_count((callback_payload_t)type);
        post_borrowed_payload((callback_payload_t)type, conn, uid, channel, custom, payload, len,
                              &info);
        memset(payload, 0xee, len);
        memset(uid, 'x', sizeof(uid) - 1);
        memset(channel, 'x', sizeof(channel) - 1);
        memset(custom, 'x', sizeof(custom) - 1);
        info.data_type = AUDIO_DATA_TYPE_G722;
        aosl_atomic_set(&s_release_state_callback, true);
        synchronize_rtc();
        assert(payload_callback_count((callback_payload_t)type) == callbacks_before + 1);
        expect_copied_payload((callback_payload_t)type, expected, len);

#ifdef MYBOT_TEST_WRAP_RTC_ALLOC
        for (unsigned fail_at = 1; fail_at <= 2; ++fail_at) {
            memcpy(uid, "original-peer", sizeof(uid));
            memcpy(channel, "payload-room", sizeof(channel));
            memcpy(custom, "original-type", sizeof(custom));
            memcpy(payload, expected, len);
            info.data_type = AUDIO_DATA_TYPE_PCM;
            block_payload_owner(conn);
            callbacks_before = payload_callback_count((callback_payload_t)type);
            callback_allocation_fault_t fault = {.fail_at = fail_at};
            int ret = pthread_setspecific(s_allocation_fault_key, &fault);
            assert(ret == 0);
            post_borrowed_payload((callback_payload_t)type, conn, uid, channel, custom, payload,
                                  len, &info);
            ret = pthread_setspecific(s_allocation_fault_key, NULL);
            assert(ret == 0 && fault.failed);
            aosl_atomic_set(&s_release_state_callback, true);
            synchronize_rtc();
            assert(payload_callback_count((callback_payload_t)type) == callbacks_before);
            post_borrowed_payload((callback_payload_t)type, conn, uid, channel, custom, payload,
                                  len, &info);
            synchronize_rtc();
            assert(payload_callback_count((callback_payload_t)type) == callbacks_before + 1);
            expect_copied_payload((callback_payload_t)type, expected, len);
        }
#endif
    }
    assert(mybot_agora_rtc_leave() == 0);
    assert(mybot_agora_rtc_fini() == 0);
}

static void test_rtm_callback_boundaries(const mybot_agora_rtc_callbacks_t *callbacks) {
    assert(mybot_agora_rtc_init("boundary-app", callbacks) == 0);
    assert(mybot_agora_rtc_join("boundary-room", "token", "user") == 0);
    join_rtm_login_thread();
    join_rtm_subscribe_thread();
    synchronize_rtc();

    char max_uid[MYBOT_RTM_UID_MAX_LEN];
    char oversized_uid[MYBOT_RTM_UID_MAX_LEN + 1];
    char max_custom[33];
    char oversized_custom[34];
    static unsigned char payload[AGORA_RTM_DATA_MAX_LEN + 1];
    memset(max_uid, 'u', sizeof(max_uid) - 1);
    max_uid[sizeof(max_uid) - 1] = '\0';
    memset(oversized_uid, 'u', sizeof(oversized_uid) - 1);
    oversized_uid[sizeof(oversized_uid) - 1] = '\0';
    memset(max_custom, 'c', sizeof(max_custom) - 1);
    max_custom[sizeof(max_custom) - 1] = '\0';
    memset(oversized_custom, 'c', sizeof(oversized_custom) - 1);
    oversized_custom[sizeof(oversized_custom) - 1] = '\0';
    memset(payload, 'p', sizeof(payload));

    /* Both send and receive accept the complete documented limits. */
    int sends_before = s_rtm_send_calls;
    assert(mybot_agora_rtc_send_rtm_data(max_uid, payload, AGORA_RTM_DATA_MAX_LEN, UINT32_MAX,
                                         max_custom) == 0);
    assert(s_rtm_send_calls == sends_before + 1);
    assert(s_rtm_msg_len == AGORA_RTM_DATA_MAX_LEN);
    assert(s_rtm_msg_id == UINT32_MAX);
    assert(strcmp(s_rtm_peer_uid, max_uid) == 0);
    assert(strcmp(s_rtm_custom_type, max_custom) == 0);

    int data_before = s_rtm_data_calls;
    int channel_data_before = s_rtm_subscribe_data_calls;
    s_rtm_handler.on_rtm_data(max_uid, payload, AGORA_RTM_DATA_MAX_LEN, RTM_MESSAGE_TYPE_STRING,
                              max_custom);
    s_rtm_handler.on_rtm_subscribe_data("boundary-room", max_uid, payload, AGORA_RTM_DATA_MAX_LEN,
                                        RTM_MESSAGE_TYPE_STRING, max_custom);
    synchronize_rtc();
    assert(s_rtm_data_calls == data_before + 1);
    assert(s_last_rtm_data_len == AGORA_RTM_DATA_MAX_LEN);
    assert(memcmp(s_last_rtm_data, payload, AGORA_RTM_DATA_MAX_LEN) == 0);
    assert(strcmp(s_last_rtm_data_uid, max_uid) == 0);
    assert(s_rtm_subscribe_data_calls == channel_data_before + 1);
    assert(s_last_rtm_subscribe_data_len == AGORA_RTM_DATA_MAX_LEN);
    assert(memcmp(s_last_rtm_subscribe_data, payload, AGORA_RTM_DATA_MAX_LEN) == 0);
    assert(strcmp(s_last_rtm_subscribe_uid, max_uid) == 0);
    assert(strcmp(s_last_rtm_custom_type, max_custom) == 0);

    /* Reject malformed borrowed fields without forwarding partial messages. */
    data_before = s_rtm_data_calls;
    channel_data_before = s_rtm_subscribe_data_calls;
    int results_before = s_rtm_send_result_calls;
    int events_before = s_rtm_event_calls;
    s_rtm_handler.on_rtm_data(NULL, payload, 1, RTM_MESSAGE_TYPE_BINARY, NULL);
    s_rtm_handler.on_rtm_data(max_uid, NULL, 1, RTM_MESSAGE_TYPE_BINARY, NULL);
    s_rtm_handler.on_rtm_data(max_uid, payload, sizeof(payload), RTM_MESSAGE_TYPE_BINARY, NULL);
    s_rtm_handler.on_rtm_data(oversized_uid, payload, 1, RTM_MESSAGE_TYPE_BINARY, NULL);
    s_rtm_handler.on_rtm_data(max_uid, payload, 1, RTM_MESSAGE_TYPE_BINARY, oversized_custom);
    s_rtm_handler.on_rtm_subscribe_data(NULL, max_uid, payload, 1, RTM_MESSAGE_TYPE_STRING, NULL);
    s_rtm_handler.on_rtm_subscribe_data("boundary-room", NULL, payload, 1, RTM_MESSAGE_TYPE_STRING,
                                        NULL);
    s_rtm_handler.on_rtm_subscribe_data("boundary-room", max_uid, NULL, 1, RTM_MESSAGE_TYPE_STRING,
                                        NULL);
    s_rtm_handler.on_rtm_subscribe_data("boundary-room", max_uid, payload, sizeof(payload),
                                        RTM_MESSAGE_TYPE_STRING, NULL);
    s_rtm_handler.on_rtm_subscribe_data("boundary-room", oversized_uid, payload, 1,
                                        RTM_MESSAGE_TYPE_STRING, NULL);
    s_rtm_handler.on_rtm_subscribe_data("boundary-room", max_uid, payload, 1,
                                        RTM_MESSAGE_TYPE_STRING, oversized_custom);
    s_rtm_handler.on_rtm_subscribe_data("boundary-room", max_uid, payload, 1,
                                        RTM_MESSAGE_TYPE_BINARY, NULL);
    s_rtm_handler.on_rtm_send_data_result(NULL, 1, RTM_MSG_STATE_RECEIVED);
    s_rtm_handler.on_rtm_send_data_result(oversized_uid, 1, RTM_MSG_STATE_RECEIVED);
    s_rtm_handler.on_rtm_event(NULL, RTM_EVENT_TYPE_LOGIN, ERR_RTM_OK);
    s_rtm_handler.on_rtm_event(NULL, RTM_EVENT_TYPE_EXIT, ERR_RTM_OK);
    s_rtm_handler.on_rtm_event(oversized_uid, RTM_EVENT_TYPE_EXIT, ERR_RTM_OK);
    s_rtm_handler.on_rtm_event("other-user", RTM_EVENT_TYPE_KICKOFF, ERR_RTM_FAILED);
    /* Deliberately inject an unknown vendor event to verify it is discarded. */
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    s_rtm_handler.on_rtm_event("user", (rtm_event_type_e)99, ERR_RTM_FAILED);
    s_rtm_handler.on_rtm_subscribe_result(NULL, ERR_RTM_OK);
    s_rtm_handler.on_rtm_subscribe_result("other-room", ERR_RTM_OK);
    synchronize_rtc();
    assert(s_rtm_data_calls == data_before);
    assert(s_rtm_subscribe_data_calls == channel_data_before);
    assert(s_rtm_send_result_calls == results_before);
    assert(s_rtm_event_calls == events_before);

    static const rtm_msg_state_e states[] = {RTM_MSG_STATE_INIT, RTM_MSG_STATE_UNREACHABLE,
                                             RTM_MSG_STATE_TIMEOUT};
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
        s_rtm_handler.on_rtm_send_data_result(max_uid, (uint32_t)i, states[i]);
        synchronize_rtc();
        assert(s_rtm_send_result_calls == results_before + (int)i + 1);
        assert(s_last_rtm_message_state == (mybot_rtm_message_state_t)states[i]);
        assert(s_last_rtm_result_msg_id == (uint32_t)i);
        assert(strcmp(s_last_rtm_send_uid, max_uid) == 0);
    }
    s_rtm_handler.on_rtm_data(max_uid, payload, 1, RTM_MESSAGE_TYPE_BINARY, NULL);
    s_rtm_handler.on_rtm_subscribe_data("boundary-room", max_uid, payload, 1,
                                        RTM_MESSAGE_TYPE_STRING, NULL);
    synchronize_rtc();
    assert(s_rtm_data_calls == data_before + 1);
    assert(s_rtm_subscribe_data_calls == channel_data_before + 1);
    assert(s_last_rtm_custom_type[0] == '\0');
    assert(mybot_agora_rtc_leave() == 0);
    assert(mybot_agora_rtc_fini() == 0);
}

static void test_callback_queue_saturation(const mybot_agora_rtc_callbacks_t *callbacks) {
    assert(mybot_agora_rtc_init("queue-app", callbacks) == 0);
    assert(mybot_agora_rtc_join("queue-room", "token", "user") == 0);
    join_rtm_login_thread();
    join_rtm_subscribe_thread();
    connection_id_t conn = s_last_conn;
    block_payload_owner(conn);
    int callbacks_before = s_rtm_data_calls;
    intptr_t delivered_before = aosl_atomic_read(&s_rtm_data_delivered);
    char payload[] = "borrowed payload";
    const char expected[] = "borrowed payload";
    /* The forwarding queue holds at most 1000 pending events. The owner stays
     * blocked while borrowed payloads overflow it, so callbacks must return. */
    const int posted = 1100;
    for (int i = 0; i < posted; ++i) {
        s_rtm_handler.on_rtm_data("peer", payload, sizeof(payload), RTM_MESSAGE_TYPE_BINARY, NULL);
    }
    memset(payload, 'x', sizeof(payload));
    aosl_atomic_set(&s_release_state_callback, true);
    /* Wait for two deliveries so the NONBLOCK queue has room for a barrier. */
    for (int elapsed = 0;
         elapsed < 1000 && aosl_atomic_read(&s_rtm_data_delivered) < delivered_before + 2;
         ++elapsed) {
        aosl_hal_msleep(1);
    }
    assert(aosl_atomic_read(&s_rtm_data_delivered) >= delivered_before + 2);
    synchronize_rtc();
    int accepted = s_rtm_data_calls - callbacks_before;
    assert(accepted > 0 && accepted < posted);
    assert(s_last_rtm_data_len == sizeof(expected));
    assert(memcmp(s_last_rtm_data, expected, sizeof(expected)) == 0);
    /* Dropping overflow events releases their storage and permits later work. */
    s_rtm_handler.on_rtm_data("peer", expected, sizeof(expected), RTM_MESSAGE_TYPE_BINARY, NULL);
    synchronize_rtc();
    assert(s_rtm_data_calls == callbacks_before + accepted + 1);
    assert(memcmp(s_last_rtm_data, expected, sizeof(expected)) == 0);
    assert(mybot_agora_rtc_leave() == 0);
    assert(mybot_agora_rtc_fini() == 0);
}

static void test_optional_callbacks(void) {
    int states_before = s_state_calls;
    int audio_before = s_remote_audio_calls;
    int events_before = s_rtm_event_calls;
    int data_before = s_rtm_data_calls;
    int channel_data_before = s_rtm_subscribe_data_calls;
    int subscribe_results_before = s_rtm_subscribe_result_calls;
    int send_results_before = s_rtm_send_result_calls;
    int token_expiries_before = s_token_expiry_calls;
    assert(mybot_agora_rtc_init("optional-app", NULL) == 0);
    assert(mybot_agora_rtc_login_rtm("user", NULL) == 0);
    join_rtm_login_thread();
    synchronize_rtc();
    assert(s_rtm_token[0] == '\0');
    static const char payload[] = "test";
    s_rtm_handler.on_rtm_data("peer", payload, sizeof(payload), RTM_MESSAGE_TYPE_STRING, NULL);
    s_rtm_handler.on_rtm_data("peer", payload, 0, RTM_MESSAGE_TYPE_STRING, NULL);
    /* Deliberately inject an unknown wire message type. */
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    s_rtm_handler.on_rtm_data("peer", payload, sizeof(payload), (rtm_message_type_e)99, NULL);
    s_rtm_handler.on_rtm_send_data_result("peer", 1, RTM_MSG_STATE_RECEIVED);
    synchronize_rtc();
    assert(mybot_agora_rtc_join("optional-room", "", "user") == 0);
    join_rtm_subscribe_thread();
    synchronize_rtc();
    connection_id_t conn = s_last_conn;
    s_handler.on_join_channel_success(conn, 42, 0);
    static unsigned char audio[RTC_REMOTE_PCM_FRAME_BYTES];
    const audio_frame_info_t info = {.data_type = AUDIO_DATA_TYPE_PCM};
    s_handler.on_audio_data(conn, 7, 0, audio, sizeof(audio), &info);
    s_handler.on_token_privilege_will_expire(conn, NULL);
    s_rtm_handler.on_rtm_subscribe_data("optional-room", "peer", payload, sizeof(payload),
                                        RTM_MESSAGE_TYPE_STRING, NULL);
    s_rtm_handler.on_rtm_subscribe_data("optional-room", "peer", payload, 0,
                                        RTM_MESSAGE_TYPE_STRING, NULL);
#if MYBOT_ENABLE_VIDEO
    s_handler.on_target_bitrate_changed(conn, 96000);
    s_handler.on_key_frame_gen_req(conn, 7, VIDEO_STREAM_HIGH);
#endif
    synchronize_rtc();
    /* A failed subscription event disables channel delivery until success. */
    s_rtm_handler.on_rtm_subscribe_result("optional-room", ERR_RTM_FAILED);
    synchronize_rtc();
    s_rtm_handler.on_rtm_subscribe_data("optional-room", "peer", payload, sizeof(payload),
                                        RTM_MESSAGE_TYPE_STRING, NULL);
    synchronize_rtc();
    s_rtm_handler.on_rtm_subscribe_result("optional-room", ERR_RTM_OK);
    synchronize_rtc();
    int logouts_before = s_rtm_logout_calls;
    assert(mybot_agora_rtc_leave() == 0);
    /* Explicit login survives RTC leave and still accepts point-to-point sends. */
    synchronize_rtc();
    assert(s_rtm_logout_calls == logouts_before);
    assert(mybot_agora_rtc_send_rtm_data("peer", payload, sizeof(payload), 1, NULL) == 0);

    s_rtm_handler.on_rtm_event("user", RTM_EVENT_TYPE_EXIT, ERR_RTM_FAILED);
    assert(!mybot_agora_rtc_is_rtm_logged_in());
    assert(mybot_agora_rtc_login_rtm("user", "") == 0);
    join_rtm_login_thread();
    synchronize_rtc();
    assert(s_rtm_token[0] == '\0');
    s_rtm_handler.on_rtm_event("user", RTM_EVENT_TYPE_EXIT, ERR_RTM_OK);
    assert(!mybot_agora_rtc_is_rtm_logged_in());
    assert(s_state_calls == states_before);
    assert(s_remote_audio_calls == audio_before);
    assert(s_rtm_event_calls == events_before);
    assert(s_rtm_data_calls == data_before);
    assert(s_rtm_subscribe_data_calls == channel_data_before);
    assert(s_rtm_subscribe_result_calls == subscribe_results_before);
    assert(s_rtm_send_result_calls == send_results_before);
    assert(s_token_expiry_calls == token_expiries_before);
    assert(mybot_agora_rtc_fini() == 0);
}

static void test_stale_connection_callbacks(const mybot_agora_rtc_callbacks_t *callbacks) {
    assert(mybot_agora_rtc_init("stale-app", callbacks) == 0);
    assert(mybot_agora_rtc_join("stale-room", "token", "user") == 0);
    join_rtm_login_thread();
    join_rtm_subscribe_thread();
    connection_id_t active = s_last_conn;
    s_handler.on_join_channel_success(active, 42, 0);
    synchronize_rtc();
    int states_before = s_state_calls;
    int audio_before = s_remote_audio_calls;
    int token_expiries_before = s_token_expiry_calls;
    connection_id_t stale = active + 100;
    user_info_t user = {.uid = 7};
    snprintf(user.user_account, sizeof(user.user_account), "%s", "remote-user");
    static unsigned char audio[RTC_REMOTE_PCM_FRAME_BYTES];
    const audio_frame_info_t info = {.data_type = AUDIO_DATA_TYPE_PCM};
    s_handler.on_reconnecting(stale);
    s_handler.on_connection_lost(stale);
    s_handler.on_rejoin_channel_success(stale, 42, 0);
    s_handler.on_user_joined_with_user_account(stale, &user, 0);
    s_handler.on_user_offline_with_user_account(stale, &user, 0);
    s_handler.on_error(stale, -1, "stale");
    s_handler.on_license_validation_failure(stale, 1);
    s_handler.on_token_privilege_will_expire(stale, NULL);
    s_handler.on_audio_data(stale, 7, 0, audio, sizeof(audio), &info);
    s_handler.on_audio_data(active, 7, 0, NULL, sizeof(audio), &info);
    s_handler.on_audio_data(active, 7, 0, audio, 0, &info);
    /* Duplicate connection success must not repeat the application state event. */
    s_handler.on_join_channel_success(active, 42, 0);
#if MYBOT_ENABLE_VIDEO
    int key_frames_before = s_video_key_frame_requests;
    uint32_t bitrate_before = s_video_target_bitrate;
    s_handler.on_target_bitrate_changed(stale, 77777);
    s_handler.on_key_frame_gen_req(stale, 7, VIDEO_STREAM_HIGH);
    s_handler.on_key_frame_gen_req(active, 7, VIDEO_STREAM_LOW);
#endif
    synchronize_rtc();
    assert(s_state_calls == states_before);
    assert(s_last_state == MYBOT_RTC_STATE_CONNECTED);
    assert(s_remote_audio_calls == audio_before);
    assert(s_token_expiry_calls == token_expiries_before);
#if MYBOT_ENABLE_VIDEO
    assert(s_video_key_frame_requests == key_frames_before);
    assert(s_video_target_bitrate == bitrate_before);
#endif
    assert(mybot_agora_rtc_leave() == 0);
    assert(mybot_agora_rtc_fini() == 0);
    states_before = s_state_calls;
    s_handler.on_join_channel_success(active, 42, 0);
    s_handler.on_audio_data(active, 7, 0, audio, sizeof(audio), &info);
    s_rtm_handler.on_rtm_event("user", RTM_EVENT_TYPE_EXIT, ERR_RTM_OK);
    s_rtm_handler.on_rtm_data("peer", audio, 1, RTM_MESSAGE_TYPE_BINARY, NULL);
    s_rtm_handler.on_rtm_subscribe_result("stale-room", ERR_RTM_OK);
    s_rtm_handler.on_rtm_subscribe_data("stale-room", "peer", audio, 1, RTM_MESSAGE_TYPE_STRING,
                                        NULL);
    s_rtm_handler.on_rtm_send_data_result("peer", 1, RTM_MSG_STATE_RECEIVED);
#if MYBOT_ENABLE_VIDEO
    s_handler.on_target_bitrate_changed(active, 77777);
    s_handler.on_key_frame_gen_req(active, 7, VIDEO_STREAM_HIGH);
#endif
    assert(s_state_calls == states_before);
    assert(s_remote_audio_calls == audio_before);
    assert(!mybot_agora_rtc_is_rtm_logged_in());
    assert(mybot_agora_rtc_login_rtm("user", NULL) < 0);
    assert(mybot_agora_rtc_logout_rtm() < 0);
    assert(mybot_agora_rtc_join("room", NULL, "user") < 0);
    assert(mybot_agora_rtc_send_audio(audio, sizeof(audio)) < 0);
    assert(mybot_agora_rtc_send_rtm_data("peer", audio, 1, 1, NULL) < 0);
    assert(mybot_agora_rtc_renew_token("token") < 0);
    assert(mybot_agora_rtc_fini() == 0);
}

static void test_fini_cleanup_failures(const mybot_agora_rtc_callbacks_t *callbacks) {
    for (int fault = 0; fault < 2; ++fault) {
        assert(mybot_agora_rtc_init("fini-app", callbacks) == 0);
        assert(mybot_agora_rtc_join("fini-room", "token", "user") == 0);
        join_rtm_login_thread();
        join_rtm_subscribe_thread();
        int finis_before = s_fini_calls;
        int destroys_before = s_destroy_calls;
        int logouts_before = s_rtm_logout_calls;
        s_leave_result = fault == 0 ? -1 : 0;
        s_rtm_logout_result = fault == 1 ? -1 : 0;
        assert(mybot_agora_rtc_fini() < 0);
        assert(s_fini_calls == finis_before + 1);
        assert(s_destroy_calls == destroys_before + 1);
        assert(s_rtm_logout_calls == logouts_before + 1);
        assert(s_destroy_seq < s_fini_seq);
        assert(!mybot_agora_rtc_is_rtm_logged_in());
        s_leave_result = 0;
        s_rtm_logout_result = 0;
        assert(mybot_agora_rtc_fini() == 0);
        assert(s_fini_calls == finis_before + 1);
    }
    /* An invalid initialization still creates an owner queue, but no vendor service. */
    int finis_before = s_fini_calls;
    assert(mybot_agora_rtc_init(NULL, NULL) < 0);
    assert(mybot_agora_rtc_fini() == 0);
    assert(s_fini_calls == finis_before);
}

#ifdef MYBOT_TEST_WRAP_RTC_ALLOC
static void test_notification_allocation_failures(const mybot_agora_rtc_callbacks_t *callbacks) {
    assert(mybot_agora_rtc_init("notification-app", callbacks) == 0);
    assert(mybot_agora_rtc_join("notification-room", "token", "user") == 0);
    join_rtm_login_thread();
    join_rtm_subscribe_thread();
    connection_id_t conn = s_last_conn;
    s_handler.on_join_channel_success(conn, 42, 0);
    synchronize_rtc();
    int states_before = s_state_calls;
    int events_before = s_rtm_event_calls;
    int send_results_before = s_rtm_send_result_calls;
    int subscribe_results_before = s_rtm_subscribe_result_calls;
    int token_expiries_before = s_token_expiry_calls;
    user_info_t user = {.uid = 7};
    snprintf(user.user_account, sizeof(user.user_account), "%s", "remote-user");
#if MYBOT_ENABLE_VIDEO
    int key_frames_before = s_video_key_frame_requests;
    uint32_t bitrate_before = s_video_target_bitrate;
    const int notification_count = 14;
#else
    const int notification_count = 12;
#endif
    for (int kind = 0; kind < notification_count; ++kind) {
        callback_allocation_fault_t fault = {.fail_at = 1};
        assert(pthread_setspecific(s_allocation_fault_key, &fault) == 0);
        switch (kind) {
        case 0:
            s_handler.on_join_channel_success(conn, 42, 0);
            break;
        case 1:
            s_handler.on_reconnecting(conn);
            break;
        case 2:
            s_handler.on_connection_lost(conn);
            break;
        case 3:
            s_handler.on_rejoin_channel_success(conn, 42, 0);
            break;
        case 4:
            s_handler.on_user_joined_with_user_account(conn, &user, 0);
            break;
        case 5:
            s_handler.on_user_offline_with_user_account(conn, &user, 0);
            break;
        case 6:
            s_handler.on_error(conn, -1, "allocation-failure");
            break;
        case 7:
            s_handler.on_license_validation_failure(conn, 1);
            break;
        case 8:
            s_handler.on_token_privilege_will_expire(conn, NULL);
            break;
        case 9:
            s_rtm_handler.on_rtm_event("user", RTM_EVENT_TYPE_LOGIN, ERR_RTM_OK);
            break;
        case 10:
            s_rtm_handler.on_rtm_send_data_result("peer", 1, RTM_MSG_STATE_RECEIVED);
            break;
        case 11:
            s_rtm_handler.on_rtm_subscribe_result("notification-room", ERR_RTM_OK);
            break;
#if MYBOT_ENABLE_VIDEO
        case 12:
            s_handler.on_target_bitrate_changed(conn, 77777);
            break;
        case 13:
            s_handler.on_key_frame_gen_req(conn, 7, VIDEO_STREAM_HIGH);
            break;
#endif
        default:
            assert(false);
        }
        assert(pthread_setspecific(s_allocation_fault_key, NULL) == 0);
        assert(fault.failed);
        synchronize_rtc();
        assert(s_state_calls == states_before);
        assert(s_last_state == MYBOT_RTC_STATE_CONNECTED);
        assert(s_rtm_event_calls == events_before);
        assert(s_rtm_send_result_calls == send_results_before);
        assert(s_rtm_subscribe_result_calls == subscribe_results_before);
        assert(s_token_expiry_calls == token_expiries_before);
#if MYBOT_ENABLE_VIDEO
        assert(s_video_key_frame_requests == key_frames_before);
        assert(s_video_target_bitrate == bitrate_before);
#endif
    }
    /* A dropped notification does not prevent later notifications or media. */
    s_handler.on_reconnecting(conn);
    synchronize_rtc();
    assert(s_last_state == MYBOT_RTC_STATE_RECONNECTING);
    s_handler.on_rejoin_channel_success(conn, 42, 0);
    s_handler.on_token_privilege_will_expire(conn, NULL);
    s_rtm_handler.on_rtm_send_data_result("peer", 1, RTM_MSG_STATE_RECEIVED);
    synchronize_rtc();
    assert(s_last_state == MYBOT_RTC_STATE_CONNECTED);
    assert(s_state_calls == states_before + 2);
    assert(s_token_expiry_calls == token_expiries_before + 1);
    assert(s_rtm_send_result_calls == send_results_before + 1);
    assert(mybot_agora_rtc_leave() == 0);
    assert(mybot_agora_rtc_fini() == 0);
}
#endif

#if MYBOT_ENABLE_VIDEO
static void test_video_boundaries(const mybot_agora_rtc_callbacks_t *callbacks) {
    assert(mybot_agora_rtc_init("video-app", callbacks) == 0);
    int logins_before = s_rtm_login_calls;
    assert((mybot_agora_rtc_join)("video-room", "token", "user", 0, 0) < 0);
    assert((mybot_agora_rtc_join)("video-room", "token", "user", 2, 1) < 0);
    assert(s_rtm_login_calls == logins_before);
    assert((mybot_agora_rtc_join)("video-room", "token", "user", 0, UINT32_MAX) == 0);
    join_rtm_login_thread();
    join_rtm_subscribe_thread();
    assert(s_last_bwe_min_bps == 0);
    assert(s_last_bwe_max_bps == UINT32_MAX);
    assert(s_last_bwe_start_bps == UINT32_MAX / 2U);
    unsigned char payload[] = {0, 0, 0, 1, 0x26};
    mybot_video_frame_t frame = {
        .data = payload, .len = sizeof(payload), .codec = MYBOT_VIDEO_CODEC_H264};
    int sends_before = s_send_calls;
    assert(mybot_agora_rtc_send_video(&frame) < 0);
    s_handler.on_join_channel_success(s_last_conn, 42, 0);
    synchronize_rtc();
    assert(mybot_agora_rtc_send_video(NULL) < 0);
    frame.data = NULL;
    assert(mybot_agora_rtc_send_video(&frame) < 0);
    frame.data = payload;
    frame.len = 0;
    assert(mybot_agora_rtc_send_video(&frame) < 0);
    frame.len = MYBOT_VIDEO_MAX_FRAME_BYTES + 1U;
    assert(mybot_agora_rtc_send_video(&frame) < 0);
    frame.len = sizeof(payload);
    frame.codec = (mybot_video_codec_t)99;
    assert(mybot_agora_rtc_send_video(&frame) < 0);
    assert(s_send_calls == sends_before);
    frame.codec = MYBOT_VIDEO_CODEC_H264;
    assert(mybot_agora_rtc_send_video(&frame) == 0);
    assert(s_last_video_info.data_type == VIDEO_DATA_TYPE_H264);
    assert(s_last_video_info.stream_type == VIDEO_STREAM_HIGH);
    assert(s_last_send_len == sizeof(payload));
    frame.codec = MYBOT_VIDEO_CODEC_H265;
    assert(mybot_agora_rtc_send_video(&frame) == 0);
    assert(s_last_video_info.data_type == VIDEO_DATA_TYPE_H265);
    s_send_result = -1;
    assert(mybot_agora_rtc_send_video(&frame) < 0);
    s_send_result = 0;
    assert(s_send_calls == sends_before + 3);
    assert(mybot_agora_rtc_leave() == 0);
    sends_before = s_send_calls;
    assert(mybot_agora_rtc_send_video(&frame) < 0);
    assert(s_send_calls == sends_before);
    assert(mybot_agora_rtc_fini() == 0);
    assert(mybot_agora_rtc_send_video(&frame) < 0);
}
#endif

int main(void) {
#ifdef MYBOT_TEST_WRAP_RTC_ALLOC
    int key_ret = pthread_key_create(&s_allocation_fault_key, NULL);
    assert(key_ret == 0);
    s_allocation_fault_key_ready = true;
#endif
    aosl_ctor();

    unsigned char pcm_frame[RTC_PCM_FRAME_BYTES] = {0};
    char oversized_app_id[65];
    memset(oversized_app_id, 'a', sizeof(oversized_app_id) - 1);
    oversized_app_id[sizeof(oversized_app_id) - 1] = '\0';
    mybot_agora_rtc_callbacks_t callbacks = {
        .on_remote_audio = on_remote_audio,
        .on_token_will_expire = on_token_will_expire,
        .on_state_changed = on_state_changed,
        .on_rtm_event = on_rtm_event,
        .on_rtm_data = on_rtm_data,
        .on_rtm_subscribe_result = on_rtm_subscribe_result,
        .on_rtm_subscribe_data = on_rtm_subscribe_data,
        .on_rtm_send_data_result = on_rtm_send_data_result,
#if MYBOT_ENABLE_VIDEO
        .on_video_key_frame_requested = on_video_key_frame_requested,
        .on_video_target_bitrate_changed = on_video_target_bitrate_changed,
#endif
        .user_data = &s_callback_owner,
    };

    /* RTM accounts follow the Agora/xiaozhi string UID contract. */
    assert(!mybot_agora_rtc_rtm_uid_is_valid(NULL));
    assert(!mybot_agora_rtc_rtm_uid_is_valid(""));
    assert(mybot_agora_rtc_rtm_uid_is_valid("device-uid 01"));
    assert(mybot_agora_rtc_rtm_uid_is_valid("!#$%&()+-:;<=>?@[]^_{|}~,"));
    char max_rtm_uid[MYBOT_RTM_UID_MAX_LEN];
    memset(max_rtm_uid, 'a', sizeof(max_rtm_uid) - 1);
    max_rtm_uid[sizeof(max_rtm_uid) - 1] = '\0';
    assert(mybot_agora_rtc_rtm_uid_is_valid(max_rtm_uid));
    char oversized_rtm_uid[MYBOT_RTM_UID_MAX_LEN + 1];
    memset(oversized_rtm_uid, 'a', sizeof(oversized_rtm_uid) - 1);
    oversized_rtm_uid[sizeof(oversized_rtm_uid) - 1] = '\0';
    assert(!mybot_agora_rtc_rtm_uid_is_valid(oversized_rtm_uid));
    assert(!mybot_agora_rtc_rtm_uid_is_valid("bad\nuid"));
    assert(!mybot_agora_rtc_rtm_uid_is_valid("bad/uid"));
    assert(!mybot_agora_rtc_rtm_uid_is_valid("bad\\uid"));

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

    aosl_set_log_level(AOSL_LOG_INFO);
    assert(mybot_agora_rtc_init("app-1", &callbacks) == 0);
    assert(aosl_get_log_level() == AOSL_LOG_INFO);
    assert(s_init_calls == 1);
    assert(s_handler.on_rtc_stats == NULL);

    /* Explicit RTM login is gated by SDK initialization and login event. */
    assert(!mybot_agora_rtc_is_rtm_logged_in());
    assert(mybot_agora_rtc_login_rtm(NULL, "token") < 0);
    assert(mybot_agora_rtc_login_rtm(oversized_rtm_uid, "token") < 0);
    assert(s_rtm_login_calls == 0);
    s_rtm_login_result = -1;
    assert(mybot_agora_rtc_login_rtm("device-uid 01", "rejected-token") < 0);
    assert(s_rtm_login_calls == 1);
    assert(!mybot_agora_rtc_is_rtm_logged_in());
    s_rtm_login_result = 0;
    assert(mybot_agora_rtc_login_rtm("device-uid 01", "rtm-token") == 0);
    assert(s_rtm_login_calls == 2);
    assert(strcmp(s_rtm_uid, "device-uid 01") == 0);
    assert(strcmp(s_rtm_token, "rtm-token") == 0);
    assert(mybot_agora_rtc_login_rtm("device-uid 01", "other-token") == 0);
    assert(s_rtm_login_calls == 2);
    assert(mybot_agora_rtc_login_rtm("other-device", "rtm-token") < 0);
    assert(s_rtm_login_calls == 2);
    assert(!mybot_agora_rtc_is_rtm_logged_in());
    assert(s_rtm_handler.on_rtm_event != NULL);
    s_rtm_handler.on_rtm_event("other-device", RTM_EVENT_TYPE_LOGIN, ERR_RTM_OK);
    assert(s_rtm_event_calls == 0);
    s_rtm_handler.on_rtm_event("device-uid 01", RTM_EVENT_TYPE_LOGIN, ERR_RTM_OK);
    assert(mybot_agora_rtc_is_rtm_logged_in());
    assert(s_rtm_event_calls == 1);
    assert(s_last_rtm_event == MYBOT_RTM_EVENT_LOGIN);
    assert(s_last_rtm_error == ERR_RTM_OK);
    assert(strcmp(s_last_rtm_event_uid, "device-uid 01") == 0);
    int rtm_events_before_login_failure = s_rtm_event_calls;
    s_rtm_handler.on_rtm_event("device-uid 01", RTM_EVENT_TYPE_LOGIN, ERR_RTM_LOGIN_REJECTED);
    assert(!mybot_agora_rtc_is_rtm_logged_in());
    assert(s_rtm_event_calls == rtm_events_before_login_failure + 1);
    assert(s_last_rtm_error == ERR_RTM_LOGIN_REJECTED);
    s_rtm_handler.on_rtm_event("device-uid 01", RTM_EVENT_TYPE_LOGIN, ERR_RTM_OK);
    assert(!mybot_agora_rtc_is_rtm_logged_in());
    assert(s_rtm_event_calls == rtm_events_before_login_failure + 1);
    int rtm_logins_before_retry = s_rtm_login_calls;
    assert(mybot_agora_rtc_login_rtm("device-uid 01", "rtm-token") == 0);
    assert(s_rtm_login_calls == rtm_logins_before_retry + 1);
    s_rtm_handler.on_rtm_event("device-uid 01", RTM_EVENT_TYPE_LOGIN, ERR_RTM_OK);
    assert(mybot_agora_rtc_is_rtm_logged_in());
    assert(s_rtm_event_calls == rtm_events_before_login_failure + 2);
    /* The RTC join account must match an already requested RTM account. */
    int rtm_logins_before_mismatched_join = s_rtm_login_calls;
    assert(mybot_agora_rtc_join("room", "token", "other-device") < 0);
    assert(s_rtm_login_calls == rtm_logins_before_mismatched_join);

    static const char rtm_payload[] = "{\"type\":\"hello\"}";
    assert(mybot_agora_rtc_send_rtm_data(NULL, rtm_payload, sizeof(rtm_payload) - 1, 1, NULL) < 0);
    assert(mybot_agora_rtc_send_rtm_data("bad/peer", rtm_payload, sizeof(rtm_payload) - 1, 1,
                                         NULL) < 0);
    assert(mybot_agora_rtc_send_rtm_data("agent-uid", NULL, sizeof(rtm_payload) - 1, 1, NULL) < 0);
    assert(mybot_agora_rtc_send_rtm_data("agent-uid", rtm_payload, 0, 1, NULL) < 0);
    char oversized_rtm_payload[AGORA_RTM_DATA_MAX_LEN + 1];
    memset(oversized_rtm_payload, 'x', sizeof(oversized_rtm_payload));
    assert(mybot_agora_rtc_send_rtm_data("agent-uid", oversized_rtm_payload,
                                         sizeof(oversized_rtm_payload), 1, NULL) < 0);
    char oversized_custom_type[34];
    memset(oversized_custom_type, 'x', sizeof(oversized_custom_type) - 1);
    oversized_custom_type[sizeof(oversized_custom_type) - 1] = '\0';
    assert(mybot_agora_rtc_send_rtm_data("agent-uid", rtm_payload, sizeof(rtm_payload) - 1, 1,
                                         oversized_custom_type) < 0);
    assert(mybot_agora_rtc_send_rtm_data("agent-uid", rtm_payload, sizeof(rtm_payload) - 1, 7,
                                         "json") == 0);
    assert(s_rtm_send_calls == 1);
    assert(strcmp(s_rtm_peer_uid, "agent-uid") == 0);
    assert(s_rtm_msg_len == sizeof(rtm_payload) - 1);
    assert(s_rtm_msg_id == 7);
    assert(s_rtm_msg_type == RTM_MESSAGE_TYPE_BINARY);
    assert(strcmp(s_rtm_custom_type, "json") == 0);
    s_rtm_send_result = -1;
    assert(mybot_agora_rtc_send_rtm_data("agent-uid", rtm_payload, sizeof(rtm_payload) - 1, 8,
                                         NULL) < 0);
    assert(s_rtm_send_calls == 2);
    s_rtm_send_result = 0;
    assert(s_rtm_handler.on_rtm_data != NULL);
    s_rtm_handler.on_rtm_data("agent-uid", rtm_payload, sizeof(rtm_payload) - 1,
                              RTM_MESSAGE_TYPE_STRING, "json");
    synchronize_rtc();
    assert(s_rtm_data_calls == 1);
    assert(strcmp(s_last_rtm_data_uid, "agent-uid") == 0);
    assert(s_last_rtm_data_len == sizeof(rtm_payload) - 1);
    assert(strcmp(s_last_rtm_custom_type, "json") == 0);
    assert(s_rtm_handler.on_rtm_send_data_result != NULL);
    s_rtm_handler.on_rtm_send_data_result("agent-uid", 7, RTM_MSG_STATE_RECEIVED);
    synchronize_rtc();
    assert(s_rtm_send_result_calls == 1);
    assert(strcmp(s_last_rtm_send_uid, "agent-uid") == 0);
    assert(s_last_rtm_result_msg_id == 7);
    assert(s_last_rtm_message_state == MYBOT_RTM_MSG_STATE_RECEIVED);
    int rtm_events_before_kickoff = s_rtm_event_calls;
    s_rtm_handler.on_rtm_event("device-uid 01", RTM_EVENT_TYPE_KICKOFF, ERR_RTM_LOGIN_REJECTED);
    assert(!mybot_agora_rtc_is_rtm_logged_in());
    assert(s_rtm_event_calls == rtm_events_before_kickoff + 1);
    assert(mybot_agora_rtc_send_rtm_data("agent-uid", rtm_payload, sizeof(rtm_payload) - 1, 8,
                                         NULL) < 0);
    int logout_calls_after_kickoff = s_rtm_logout_calls;
    assert(mybot_agora_rtc_logout_rtm() == 0);
    assert(s_rtm_logout_calls == logout_calls_after_kickoff);

    assert(mybot_agora_rtc_login_rtm("device-uid 01", "rtm-token") == 0);
    s_rtm_handler.on_rtm_event("device-uid 01", RTM_EVENT_TYPE_LOGIN, ERR_RTM_OK);
    s_rtm_logout_result = -1;
    assert(mybot_agora_rtc_logout_rtm() < 0);
    assert(s_rtm_logout_calls == logout_calls_after_kickoff + 1);
    assert(!mybot_agora_rtc_is_rtm_logged_in());
    s_rtm_logout_result = 0;
    assert(mybot_agora_rtc_logout_rtm() == 0);
    /* Failed logout clears local state, so a retry does not call the vendor. */
    assert(s_rtm_logout_calls == logout_calls_after_kickoff + 1);
    int rtm_events_after_logout = s_rtm_event_calls;
    s_rtm_handler.on_rtm_event("device-uid 01", RTM_EVENT_TYPE_LOGIN, ERR_RTM_OK);
    s_rtm_handler.on_rtm_data("agent-uid", rtm_payload, sizeof(rtm_payload) - 1,
                              RTM_MESSAGE_TYPE_STRING, "json");
    s_rtm_handler.on_rtm_send_data_result("agent-uid", 7, RTM_MSG_STATE_RECEIVED);
    assert(s_rtm_event_calls == rtm_events_after_logout);
    assert(s_rtm_data_calls == 1);
    assert(s_rtm_send_result_calls == 1);

    assert(mybot_agora_rtc_init("app-1", NULL) == 0);
    assert(mybot_agora_rtc_init("app-1", &callbacks) == 0);
    assert(s_init_calls == 1);
    assert(mybot_agora_rtc_init("app-2", &callbacks) < 0);
    assert(s_init_calls == 1);

    assert(mybot_agora_rtc_join(NULL, "token", "user") < 0);
    assert(mybot_agora_rtc_join("", "token", "user") < 0);
    assert(mybot_agora_rtc_join("room", "token", NULL) < 0);

    s_auto_rtm_login_event = true;
    s_auto_rtm_login_error = ERR_RTM_LOGIN_REJECTED;
    int creates_before_rtm_failure = s_create_calls;
    int joins_before_rtm_failure = s_join_calls;
    int logouts_before_rtm_failure = s_rtm_logout_calls;
    int subscribes_before_rtm_failure = s_rtm_subscribe_calls;
    assert(mybot_agora_rtc_join("room", "token", "user") < 0);
    assert(s_create_calls == creates_before_rtm_failure);
    assert(s_join_calls == joins_before_rtm_failure);
    assert(s_rtm_logout_calls == logouts_before_rtm_failure);
    assert(s_rtm_subscribe_calls == subscribes_before_rtm_failure);

    s_auto_rtm_login_event = false;
    int creates_before_rtm_timeout = s_create_calls;
    int joins_before_rtm_timeout = s_join_calls;
    int logouts_before_rtm_timeout = s_rtm_logout_calls;
    int subscribes_before_rtm_timeout = s_rtm_subscribe_calls;
    assert(mybot_agora_rtc_join("room", "token", "user") < 0);
    assert(s_create_calls == creates_before_rtm_timeout);
    assert(s_join_calls == joins_before_rtm_timeout);
    assert(s_rtm_logout_calls == logouts_before_rtm_timeout + 1);
    assert(s_rtm_subscribe_calls == subscribes_before_rtm_timeout);

    s_auto_rtm_login_event = true;
    s_auto_rtm_login_error = ERR_RTM_OK;
    s_rtm_subscribe_result = -1;
    int creates_before_sync_subscribe_failure = s_create_calls;
    int joins_before_sync_subscribe_failure = s_join_calls;
    int subscribes_before_sync_failure = s_rtm_subscribe_calls;
    int unsubscribes_before_sync_failure = s_rtm_unsubscribe_calls;
    assert(mybot_agora_rtc_join("room", "token", "user") < 0);
    assert(s_rtm_subscribe_calls == subscribes_before_sync_failure + 1);
    assert(s_rtm_unsubscribe_calls == unsubscribes_before_sync_failure);
    assert(s_create_calls == creates_before_sync_subscribe_failure);
    assert(s_join_calls == joins_before_sync_subscribe_failure);
    s_rtm_subscribe_result = 0;

    s_auto_rtm_subscribe_event = true;
    s_auto_rtm_subscribe_error = ERR_RTM_FAILED;
    int creates_before_subscribe_failure = s_create_calls;
    int joins_before_subscribe_failure = s_join_calls;
    int subscribes_before_failure = s_rtm_subscribe_calls;
    assert(mybot_agora_rtc_join("room", "token", "user") < 0);
    assert(s_rtm_subscribe_calls == subscribes_before_failure + 1);
    assert(s_create_calls == creates_before_subscribe_failure);
    assert(s_join_calls == joins_before_subscribe_failure);

    s_auto_rtm_subscribe_event = false;
    int creates_before_subscribe_timeout = s_create_calls;
    int joins_before_subscribe_timeout = s_join_calls;
    int unsubscribes_before_subscribe_timeout = s_rtm_unsubscribe_calls;
    assert(mybot_agora_rtc_join("room", "token", "user") < 0);
    assert(s_create_calls == creates_before_subscribe_timeout);
    assert(s_join_calls == joins_before_subscribe_timeout);
    assert(s_rtm_unsubscribe_calls == unsubscribes_before_subscribe_timeout + 1);

    s_rtm_unsubscribe_result = -1;
    int logouts_before_unsubscribe_failure = s_rtm_logout_calls;
    int unsubscribes_before_unsubscribe_failure = s_rtm_unsubscribe_calls;
    assert(mybot_agora_rtc_join("room", "token", "user") < 0);
    assert(s_rtm_unsubscribe_calls == unsubscribes_before_unsubscribe_failure + 1);
    assert(s_rtm_logout_calls == logouts_before_unsubscribe_failure + 1);
    s_rtm_unsubscribe_result = 0;

    s_auto_rtm_subscribe_event = true;
    s_auto_rtm_subscribe_error = ERR_RTM_OK;

    int rtm_logins_before_create_failure = s_rtm_login_calls;
    int rtm_logouts_before_create_failure = s_rtm_logout_calls;
    int rtm_unsubscribes_before_create_failure = s_rtm_unsubscribe_calls;
    s_create_result = -1;
    assert(mybot_agora_rtc_join("room", "token", "user") < 0);
    assert(s_rtm_login_calls == rtm_logins_before_create_failure + 1);
    assert(strcmp(s_rtm_uid, "user") == 0);
    assert(strcmp(s_rtm_token, "token") == 0);
    assert(s_rtm_logout_calls == rtm_logouts_before_create_failure + 1);
    assert(s_rtm_unsubscribe_calls == rtm_unsubscribes_before_create_failure + 1);
    s_create_result = 0;

    int rtm_logins_before_join_failure = s_rtm_login_calls;
    int rtm_logouts_before_join_failure = s_rtm_logout_calls;
    s_join_result = -1;
    assert(mybot_agora_rtc_join("room", "token", "user") < 0);
    assert(s_destroy_calls == 1);
    assert(s_rtm_login_calls == rtm_logins_before_join_failure + 1);
    assert(s_rtm_logout_calls == rtm_logouts_before_join_failure + 1);
    s_join_result = 0;

    int rtm_logins_before_join = s_rtm_login_calls;
    s_bwe_result = -1;
    assert(mybot_agora_rtc_join("room", "token", "user") == 0);
    join_rtm_login_thread();
    join_rtm_subscribe_thread();
    synchronize_rtc();
    assert(s_rtm_login_calls == rtm_logins_before_join + 1);
    assert(strcmp(s_rtm_uid, "user") == 0);
    assert(strcmp(s_rtm_token, "token") == 0);
    assert(strcmp(s_rtm_channel, "room") == 0);
    assert(s_rtm_handler.on_rtm_subscribe_result != NULL);
    assert(s_rtm_handler.on_rtm_subscribe_data != NULL);
    assert(s_last_rtm_subscribe_error == ERR_RTM_OK);
#if MYBOT_ENABLE_VIDEO
    assert(s_last_bwe_min_bps == TEST_VIDEO_MIN_BPS);
    assert(s_last_bwe_max_bps == TEST_VIDEO_MAX_BPS);
    assert(s_last_bwe_start_bps ==
           TEST_VIDEO_MIN_BPS + (TEST_VIDEO_MAX_BPS - TEST_VIDEO_MIN_BPS) / 2U);
#endif
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
#if MYBOT_ENABLE_VIDEO
    assert(!s_join_options.auto_subscribe_video);
    s_handler.on_target_bitrate_changed(first_conn, 123456);
    s_handler.on_key_frame_gen_req(first_conn, 42, VIDEO_STREAM_HIGH);
    synchronize_rtc();
    assert(s_video_target_bitrate == 123456);
    assert(s_video_key_frame_requests == 1);
#endif

    int states_before_wrong_conn = s_state_calls;
    s_handler.on_join_channel_success(first_conn + 100, 42, 0);
    synchronize_rtc();
    assert(s_state_calls == states_before_wrong_conn);
    s_handler.on_join_channel_success(first_conn, 42, 10);
    synchronize_rtc();
    assert(s_last_state == MYBOT_RTC_STATE_CONNECTED);
#if MYBOT_ENABLE_VIDEO
    unsigned char video_data[] = {0xff, 0xd8, 0xff, 0xd9};
    mybot_video_frame_t video_frame = {
        .data = video_data,
        .len = sizeof(video_data),
        .codec = MYBOT_VIDEO_CODEC_JPEG,
    };
    int sends_before_video = s_send_calls;
    assert(mybot_agora_rtc_send_video(&video_frame) == 0);
    assert(s_send_calls == sends_before_video + 1);
    assert(s_last_video_info.data_type == VIDEO_DATA_TYPE_GENERIC_JPEG);
    assert(s_last_video_info.frame_type == VIDEO_FRAME_AUTO_DETECT);
    assert(s_last_video_info.frame_rate == 0);
    assert(s_last_video_info.rotation == VIDEO_ORIENTATION_0);
#endif
    s_handler.on_reconnecting(first_conn);
    synchronize_rtc();
    assert(s_last_state == MYBOT_RTC_STATE_RECONNECTING);
    s_handler.on_connection_lost(first_conn);
    synchronize_rtc();
    assert(s_last_state == MYBOT_RTC_STATE_DISCONNECTED);
    assert(mybot_agora_rtc_send_audio(pcm_frame, sizeof(pcm_frame)) < 0);
    s_handler.on_rejoin_channel_success(first_conn, 42, 10);
    synchronize_rtc();
    assert(s_last_state == MYBOT_RTC_STATE_CONNECTED);

    int channel_data_before_wrong_channel = s_rtm_subscribe_data_calls;
    s_rtm_handler.on_rtm_subscribe_data("other-room", "agent-uid", rtm_payload,
                                        sizeof(rtm_payload) - 1, RTM_MESSAGE_TYPE_STRING, "json");
    synchronize_rtc();
    assert(s_rtm_subscribe_data_calls == channel_data_before_wrong_channel);
    s_rtm_handler.on_rtm_subscribe_data("room", "agent-uid", rtm_payload, sizeof(rtm_payload) - 1,
                                        RTM_MESSAGE_TYPE_STRING, "json");
    synchronize_rtc();
    assert(s_rtm_subscribe_data_calls == channel_data_before_wrong_channel + 1);
    assert(strcmp(s_last_rtm_subscribe_channel, "room") == 0);
    assert(strcmp(s_last_rtm_subscribe_uid, "agent-uid") == 0);
    assert(s_last_rtm_subscribe_data_len == sizeof(rtm_payload) - 1);

    user_info_t user = {0};
    user.uid = 7;
    snprintf(user.user_account, sizeof(user.user_account), "%s", "remote-user");
    s_handler.on_user_joined_with_user_account(first_conn, NULL, 0);
    s_handler.on_user_joined_with_user_account(first_conn, &user, 0);
    s_handler.on_user_offline_with_user_account(first_conn, NULL, 0);
    s_handler.on_user_offline_with_user_account(first_conn, &user, 0);
    s_handler.on_error(first_conn, -1, NULL);
    synchronize_rtc();
    assert(s_last_state == MYBOT_RTC_STATE_ERROR);
    s_handler.on_join_channel_success(first_conn, 42, 0);
    synchronize_rtc();
    int states_before_global_error = s_state_calls;
    s_handler.on_error(CONNECTION_ID_ALL, -1, "global");
    synchronize_rtc();
    assert(s_state_calls == states_before_global_error);
    s_handler.on_license_validation_failure(first_conn, 1);
    synchronize_rtc();
    assert(s_last_state == MYBOT_RTC_STATE_ERROR);
    s_handler.on_join_channel_success(first_conn, 42, 0);
    synchronize_rtc();
    s_handler.on_token_privilege_will_expire(first_conn, "old-token");
    audio_frame_info_t pcm_info = {.data_type = AUDIO_DATA_TYPE_PCM};
    unsigned char pcm_data[RTC_REMOTE_PCM_FRAME_BYTES] = {0};
    unsigned char oversized_pcm_data[RTC_REMOTE_PCM_FRAME_BYTES + sizeof(int16_t)] = {0};
    int audio_before_invalid = s_remote_audio_calls;
    s_handler.on_audio_data(first_conn, 7, 0, pcm_data, sizeof(pcm_data), NULL);
    s_handler.on_audio_data(first_conn, 7, 0, pcm_data, sizeof(pcm_data),
                            &(audio_frame_info_t){.data_type = AUDIO_DATA_TYPE_G722});
    s_handler.on_audio_data(first_conn, 7, 0, pcm_data, sizeof(pcm_data) - 1, &pcm_info);
    s_handler.on_audio_data(first_conn, 7, 0, pcm_data, sizeof(pcm_data) - 2, &pcm_info);
    s_handler.on_audio_data(first_conn, 7, 0, oversized_pcm_data, sizeof(oversized_pcm_data),
                            &pcm_info);
    synchronize_rtc();
    assert(s_remote_audio_calls == audio_before_invalid);
    s_handler.on_audio_data(first_conn, 7, 0, pcm_data, sizeof(pcm_data), &pcm_info);
    synchronize_rtc();
    assert(s_token_expiry_calls == 1);
    assert(s_remote_audio_calls == audio_before_invalid + 1);

    assert(mybot_agora_rtc_send_audio(pcm_frame, 0) < 0);
    s_send_result = -1;
    assert(mybot_agora_rtc_send_audio(pcm_frame, sizeof(pcm_frame)) < 0);
    s_send_result = 0;
    assert(mybot_agora_rtc_send_audio(pcm_frame, sizeof(pcm_frame)) == 0);
#if MYBOT_ENABLE_VIDEO
    assert(s_send_calls == 3);
#else
    assert(s_send_calls == 2);
#endif
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
    int unsubscribes_before_leave = s_rtm_unsubscribe_calls;
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
    assert((intptr_t)thread_result < 0);
    assert(s_leave_seq < s_destroy_seq);
    assert(s_rtm_unsubscribe_calls == unsubscribes_before_leave + 1);
    s_leave_result = 0;
    aosl_atomic_set(&s_block_send, false);
    assert(mybot_agora_rtc_leave() == 0);
    assert(mybot_agora_rtc_renew_token("renewed-token") < 0);

    int state_after_leave = s_state_calls;
    int audio_after_leave = s_remote_audio_calls;
    int channel_data_after_leave = s_rtm_subscribe_data_calls;
    s_handler.on_join_channel_success(first_conn, 42, 0);
    s_handler.on_audio_data(first_conn, 7, 0, pcm_data, sizeof(pcm_data), &pcm_info);
    s_rtm_handler.on_rtm_subscribe_data("room", "agent-uid", rtm_payload, sizeof(rtm_payload) - 1,
                                        RTM_MESSAGE_TYPE_STRING, "json");
    assert(!mybot_agora_rtc_is_rtm_logged_in());
    assert(s_state_calls == state_after_leave);
    assert(s_remote_audio_calls == audio_after_leave);
    assert(s_rtm_subscribe_data_calls == channel_data_after_leave);

    assert(mybot_agora_rtc_init("app-1", &callbacks) == 0);
    assert(s_init_calls == 1);
    assert(mybot_agora_rtc_join("room-2", "token", "user") == 0);
    assert(strcmp(s_rtm_channel, "room-2") == 0);
    connection_id_t second_conn = s_last_conn;
    assert(second_conn != first_conn);
    s_handler.on_join_channel_success(second_conn, 42, 0);
    join_rtm_login_thread();
    join_rtm_subscribe_thread();
    synchronize_rtc();

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
    int rtm_logouts_before_fini = s_rtm_logout_calls;
    aosl_atomic_set(&s_probe_fini_callback, true);
    mybot_agora_rtc_fini();
    assert(s_fini_calls == 1);
    assert(s_leave_calls == leaves_before_fini + 1);
    assert(s_destroy_calls == destroys_before_fini + 1);
    assert(s_rtm_logout_calls == rtm_logouts_before_fini + 1);
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
    aosl_set_log_level(AOSL_LOG_DEBUG);
    int init_calls_before_retry = s_init_calls;
    int fini_calls_before_retry = s_fini_calls;
    assert(mybot_agora_rtc_init("app-fail", &callbacks) < 0);
    assert(aosl_get_log_level() == AOSL_LOG_DEBUG);
    assert(s_init_calls == init_calls_before_retry + 1);
    assert(s_fini_calls == fini_calls_before_retry);
    s_init_result = 0;
    assert(mybot_agora_rtc_init("app-fail", &callbacks) == 0);
    assert(aosl_get_log_level() == AOSL_LOG_DEBUG);
    assert(s_init_calls == init_calls_before_retry + 2);
    mybot_agora_rtc_fini();
    assert(s_fini_calls == fini_calls_before_retry + 1);

    test_callback_payload_ownership(&callbacks);
    test_rtm_callback_boundaries(&callbacks);
    test_callback_queue_saturation(&callbacks);
    test_optional_callbacks();
    test_stale_connection_callbacks(&callbacks);
    test_fini_cleanup_failures(&callbacks);
#ifdef MYBOT_TEST_WRAP_RTC_ALLOC
    test_notification_allocation_failures(&callbacks);
#endif
#if MYBOT_ENABLE_VIDEO
    test_video_boundaries(&callbacks);
#endif

    join_rtm_login_thread();
    join_rtm_subscribe_thread();
    aosl_dtor();
#ifdef MYBOT_TEST_WRAP_RTC_ALLOC
    s_allocation_fault_key_ready = false;
    key_ret = pthread_key_delete(s_allocation_fault_key);
    assert(key_ret == 0);
#endif
    puts("agora_rtc_test: ok");
    return 0;
}
