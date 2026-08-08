/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_rtc_session.h"

#include "agora_rtc_api.h"
#include <api/aosl.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- Agora SDK stubs: capture the event handler and count calls. The real
 * x86_64 archive is not linked into this test; only the wrapper logic in
 * mybot_agora_rtc.c is exercised. ---- */
static agora_rtc_event_handler_t s_handler;
static connection_id_t s_next_conn = 1;
static int s_init_calls;
static int s_fini_calls;
static int s_join_calls;
static int s_leave_calls;
static int s_destroy_calls;
static int s_create_calls;
static int s_send_calls;
static int s_remote_audio_calls;
static int s_state_changes;
static mybot_rtc_state_t s_last_state;

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
    return 0;
}

int agora_rtc_fini(void) {
    s_fini_calls++;
    return 0;
}

int agora_rtc_create_connection(connection_id_t *conn_id) {
    s_create_calls++;
    *conn_id = s_next_conn++;
    return 0;
}

int agora_rtc_destroy_connection(connection_id_t conn_id) {
    (void)conn_id;
    s_destroy_calls++;
    return 0;
}

int agora_rtc_join_channel_with_user_account(connection_id_t conn_id, const char *channel_name,
                                             const char *user_account, const char *token,
                                             rtc_channel_options_t *options) {
    (void)conn_id;
    (void)channel_name;
    (void)user_account;
    (void)token;
    (void)options;
    s_join_calls++;
    return 0;
}

int agora_rtc_leave_channel(connection_id_t conn_id) {
    (void)conn_id;
    s_leave_calls++;
    return 0;
}

int agora_rtc_set_bwe_param(connection_id_t conn_id, uint32_t min_bps, uint32_t max_bps,
                            uint32_t start_bps) {
    (void)conn_id;
    (void)min_bps;
    (void)max_bps;
    (void)start_bps;
    return 0;
}

int agora_rtc_send_audio_data(connection_id_t conn_id, const void *data_ptr, size_t data_len,
                              audio_frame_info_t *info_ptr) {
    (void)conn_id;
    (void)data_ptr;
    (void)data_len;
    (void)info_ptr;
    s_send_calls++;
    return 0;
}

/* ---- app callbacks ---- */
static void on_state_changed(mybot_rtc_state_t state) {
    s_last_state = state;
    s_state_changes++;
}

static void on_remote_audio(uint32_t uid, const void *data, size_t len) {
    (void)uid;
    (void)data;
    (void)len;
    s_remote_audio_calls++;
}

int main(void) {
    aosl_ctor();

    /* Not initialized yet. */
    assert(mybot_rtc_session_get_state() == MYBOT_RTC_STATE_IDLE);
    assert(!mybot_rtc_session_is_connected());
    assert(mybot_rtc_session_join("room", "token", "user") < 0);
    assert(mybot_rtc_session_leave() == 0);
    assert(!mybot_rtc_session_fini()); /* no agora_rtc_fini when uninitialized */
    assert(s_fini_calls == 0);

    mybot_rtc_session_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_state_changed = on_state_changed;
    cbs.on_remote_audio = on_remote_audio;

    assert(mybot_rtc_session_init("test-app", &cbs) == 0);
    assert(s_init_calls == 1);
    assert(mybot_rtc_session_get_state() == MYBOT_RTC_STATE_INITIALIZED);

    /* Double init is a no-op. */
    assert(mybot_rtc_session_init("test-app", &cbs) == 0);
    assert(s_init_calls == 1);

    /* Cannot send before joining. */
    assert(mybot_rtc_session_send_audio("pcm", 3) < 0);
    assert(s_send_calls == 0);

    /* Join: connecting until the SDK reports success. */
    assert(mybot_rtc_session_join("room", "token", "user1") == 0);
    assert(s_join_calls == 1);
    assert(s_create_calls == 1);
    assert(mybot_rtc_session_get_state() == MYBOT_RTC_STATE_CONNECTING);
    assert(!mybot_rtc_session_is_connected());

    s_handler.on_join_channel_success(1, 42, 100);
    assert(mybot_rtc_session_is_connected());
    assert(mybot_rtc_session_get_state() == MYBOT_RTC_STATE_CONNECTED);

    assert(mybot_rtc_session_send_audio("pcm", 3) == 0);
    assert(s_send_calls == 1);

    /* Connection loss blocks sends until the rejoin callback. */
    s_handler.on_connection_lost(1);
    assert(mybot_rtc_session_get_state() == MYBOT_RTC_STATE_DISCONNECTED);
    assert(mybot_rtc_session_send_audio("pcm", 3) < 0);
    s_handler.on_rejoin_channel_success(1, 42, 50);
    assert(mybot_rtc_session_is_connected());
    assert(mybot_rtc_session_send_audio("pcm", 3) == 0);
    assert(s_send_calls == 2);

    /* License failure moves to ERROR. */
    s_handler.on_license_validation_failure(1, 2);
    assert(mybot_rtc_session_get_state() == MYBOT_RTC_STATE_ERROR);
    assert(!mybot_rtc_session_is_connected());

    /* Recover to CONNECTED, then a second join is rejected while joined. */
    s_handler.on_join_channel_success(1, 42, 0);
    assert(mybot_rtc_session_is_connected());
    assert(mybot_rtc_session_join("room2", "token", "user2") < 0);

    /* Remote audio forwards to the application callback. */
    s_handler.on_audio_data(1, 7, 0, "audio", 5, NULL);
    assert(s_remote_audio_calls == 1);

    /* Leave tears down the connection and returns to INITIALIZED. */
    assert(mybot_rtc_session_leave() == 0);
    assert(s_leave_calls == 1);
    assert(s_destroy_calls == 1);
    assert(mybot_rtc_session_get_state() == MYBOT_RTC_STATE_INITIALIZED);
    assert(mybot_rtc_session_leave() == 0); /* idempotent without a connection */
    assert(s_leave_calls == 1);

    /* Finalize calls agora_rtc_fini exactly once. */
    assert(mybot_rtc_session_fini() == true);
    assert(s_fini_calls == 1);
    assert(!mybot_rtc_session_is_connected());
    assert(mybot_rtc_session_join("room", "token", "user") < 0);
    assert(mybot_rtc_session_leave() == 0);
    assert(!mybot_rtc_session_fini()); /* now uninitialized again */
    assert(s_fini_calls == 1);
    assert(s_state_changes > 0);

    aosl_dtor();
    puts("rtc_session_test: ok");
    return 0;
}
