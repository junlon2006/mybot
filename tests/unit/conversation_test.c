/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_conversation.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static mybot_agora_rtc_callbacks_t s_rtc_callbacks;
static int s_init_result;
static int s_join_result;
static int s_leave_result;
static int s_send_result;
static int s_renew_result;
static int s_init_calls;
static int s_join_calls;
static int s_fini_calls;
static int s_remote_audio_calls;
static int s_state_calls;
static int s_expiry_calls;
static void *s_callback_owner;

int mybot_agora_rtc_init(const char *app_id, const mybot_agora_rtc_callbacks_t *callbacks) {
    assert(app_id != NULL);
    assert(callbacks != NULL);
    s_rtc_callbacks = *callbacks;
    s_init_calls++;
    return s_init_result;
}

int mybot_agora_rtc_join(const char *channel, const char *token, const char *user_account) {
    if (!channel || !token || !user_account) {
        return -1;
    }
    s_join_calls++;
    return s_join_result;
}

int mybot_agora_rtc_leave(void) {
    return s_leave_result;
}

void mybot_agora_rtc_fini(void) {
    s_fini_calls++;
}

int mybot_agora_rtc_send_audio(const void *data, size_t len) {
    assert(data != NULL);
    assert(len > 0);
    return s_send_result;
}

int mybot_agora_rtc_renew_token(const char *token) {
    assert(token != NULL);
    return s_renew_result;
}

static void on_remote_audio(uint32_t uid, const void *data, size_t len, void *user_data) {
    assert(uid == 7);
    assert(data != NULL);
    assert(len == 4);
    assert(user_data == s_callback_owner);
    s_remote_audio_calls++;
}

static void on_state_changed(mybot_rtc_state_t state, void *user_data) {
    assert(state == MYBOT_RTC_STATE_CONNECTED);
    assert(user_data == s_callback_owner);
    s_state_calls++;
}

static void on_token_will_expire(void *user_data) {
    assert(user_data == s_callback_owner);
    s_expiry_calls++;
}

static mybot_conversation_params_t params(void) {
    mybot_conversation_params_t value;
    memset(&value, 0, sizeof(value));
    snprintf(value.rtc_app_id, sizeof(value.rtc_app_id), "%s", "app-id");
    snprintf(value.rtc_channel, sizeof(value.rtc_channel), "%s", "channel");
    snprintf(value.rtc_token, sizeof(value.rtc_token), "%s", "token");
    snprintf(value.rtc_uid, sizeof(value.rtc_uid), "%s", "uid");
    return value;
}

int main(void) {
    mybot_conversation_t conversation;
    memset(&conversation, 0, sizeof(conversation));
    mybot_conversation_params_t value = params();

    assert(mybot_conversation_start(NULL, &value, NULL) < 0);
    assert(mybot_conversation_start(&conversation, NULL, NULL) < 0);
    assert(mybot_conversation_stop(NULL) < 0);
    assert(mybot_conversation_send_audio(NULL, "pcm", 3) < 0);
    assert(mybot_conversation_renew_token(NULL, "token") < 0);
    assert(mybot_conversation_renew_token(&conversation, NULL) < 0);
    mybot_conversation_fini(NULL);

    s_init_result = -1;
    assert(mybot_conversation_start(&conversation, &value, NULL) < 0);
    assert(s_init_calls == 1);
    assert(s_join_calls == 0);
    assert(s_rtc_callbacks.on_remote_audio != NULL);
    assert(conversation.cbs.on_remote_audio == NULL);

    mybot_conversation_callbacks_t callbacks = {
        .on_remote_audio = on_remote_audio,
        .on_state_changed = on_state_changed,
        .on_token_will_expire = on_token_will_expire,
        .user_data = &conversation,
    };
    s_callback_owner = &conversation;
    s_init_result = 0;
    s_join_result = -1;
    assert(mybot_conversation_start(&conversation, &value, &callbacks) < 0);
    assert(s_init_calls == 2);
    assert(s_join_calls == 1);
    assert(s_fini_calls == 0);

    s_join_result = 0;
    assert(mybot_conversation_start(&conversation, &value, &callbacks) == 0);
    assert(s_init_calls == 3);
    assert(strcmp(conversation.app_id, "app-id") == 0);
    assert(strcmp(conversation.channel, "channel") == 0);
    assert(strcmp(conversation.token, "token") == 0);
    assert(strcmp(conversation.uid, "uid") == 0);

    const char audio[] = "pcm";
    s_rtc_callbacks.on_remote_audio(7, audio, sizeof(audio), s_rtc_callbacks.user_data);
    s_rtc_callbacks.on_state_changed(MYBOT_RTC_STATE_CONNECTED, s_rtc_callbacks.user_data);
    s_rtc_callbacks.on_token_will_expire(s_rtc_callbacks.user_data);
    assert(s_remote_audio_calls == 1);
    assert(s_state_calls == 1);
    assert(s_expiry_calls == 1);

    s_send_result = -1;
    assert(mybot_conversation_send_audio(&conversation, audio, sizeof(audio)) < 0);
    s_send_result = 0;
    assert(mybot_conversation_send_audio(&conversation, audio, sizeof(audio)) == 0);

    s_renew_result = -1;
    assert(mybot_conversation_renew_token(&conversation, "rejected") < 0);
    assert(strcmp(conversation.token, "token") == 0);
    s_renew_result = 0;
    assert(mybot_conversation_renew_token(&conversation, "renewed") == 0);
    assert(strcmp(conversation.token, "renewed") == 0);

    s_leave_result = -1;
    assert(mybot_conversation_stop(&conversation) < 0);
    s_leave_result = 0;
    assert(mybot_conversation_stop(&conversation) == 0);

    mybot_conversation_fini(&conversation);
    assert(s_fini_calls == 1);
    assert(conversation.app_id[0] == '\0');
    assert(conversation.channel[0] == '\0');
    assert(conversation.token[0] == '\0');
    assert(conversation.uid[0] == '\0');
    assert(conversation.cbs.on_remote_audio == NULL);
    return 0;
}
