/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_conversation.h"

#include <api/aosl_log.h>

#include <stdio.h>
#include <string.h>

static void rtc_on_remote_audio(uint32_t uid, const void *data, size_t len, void *user_data) {
    mybot_conversation_t *conversation = user_data;
    if (conversation->cbs.on_remote_audio) {
        conversation->cbs.on_remote_audio(uid, data, len, conversation->cbs.user_data);
    }
}

static void rtc_on_state_changed(mybot_rtc_state_t state, void *user_data) {
    mybot_conversation_t *conversation = user_data;
    if (conversation->cbs.on_state_changed) {
        conversation->cbs.on_state_changed(state, conversation->cbs.user_data);
    }
}

static void rtc_on_token_will_expire(void *user_data) {
    mybot_conversation_t *conversation = user_data;
    if (conversation->cbs.on_token_will_expire) {
        conversation->cbs.on_token_will_expire(conversation->cbs.user_data);
    }
}

int mybot_conversation_start(mybot_conversation_t *conversation,
                             const mybot_conversation_params_t *params,
                             const mybot_conversation_callbacks_t *callbacks) {
    if (!conversation || !params) {
        AOSL_LOG_ERR("invalid conversation start arguments");
        return -1;
    }

    if (callbacks) {
        conversation->cbs = *callbacks;
    } else {
        memset(&conversation->cbs, 0, sizeof(conversation->cbs));
    }

    snprintf(conversation->app_id, sizeof(conversation->app_id), "%s", params->rtc_app_id);
    snprintf(conversation->channel, sizeof(conversation->channel), "%s", params->rtc_channel);
    snprintf(conversation->token, sizeof(conversation->token), "%s", params->rtc_token);
    snprintf(conversation->uid, sizeof(conversation->uid), "%s", params->rtc_uid);

    mybot_agora_rtc_callbacks_t rtc_cbs;
    memset(&rtc_cbs, 0, sizeof(rtc_cbs));
    rtc_cbs.on_remote_audio = rtc_on_remote_audio;
    rtc_cbs.on_token_will_expire = rtc_on_token_will_expire;
    rtc_cbs.on_state_changed = rtc_on_state_changed;
    rtc_cbs.user_data = conversation;

    int ret = mybot_agora_rtc_init(conversation->app_id, &rtc_cbs);
    if (ret < 0) {
        AOSL_LOG_ERR("mybot_agora_rtc_init failed");
        return ret;
    }

    AOSL_LOG_NTC("joining RTC channel=%s uid=%s", conversation->channel, conversation->uid);
    ret = mybot_agora_rtc_join(conversation->channel, conversation->token, conversation->uid);
    if (ret < 0) {
        AOSL_LOG_ERR("mybot_agora_rtc_join failed");
        return ret;
    }

    AOSL_LOG_NTC("RTC join requested");
    return 0;
}

int mybot_conversation_stop(mybot_conversation_t *conversation) {
    if (!conversation) {
        AOSL_LOG_ERR("invalid conversation stop argument");
        return -1;
    }

    AOSL_LOG_NTC("leaving RTC channel=%s uid=%s", conversation->channel, conversation->uid);
    return mybot_agora_rtc_leave();
}

int mybot_conversation_send_audio(mybot_conversation_t *conversation, const void *data,
                                  size_t len) {
    if (!conversation) {
        AOSL_LOG_ERR("invalid conversation audio argument");
        return -1;
    }
    return mybot_agora_rtc_send_audio(data, len);
}

int mybot_conversation_renew_token(mybot_conversation_t *conversation, const char *token) {
    if (!conversation || !token) {
        AOSL_LOG_ERR("invalid conversation token argument");
        return -1;
    }

    int ret = mybot_agora_rtc_renew_token(token);
    if (ret == 0) {
        snprintf(conversation->token, sizeof(conversation->token), "%s", token);
    }
    return ret;
}

void mybot_conversation_fini(mybot_conversation_t *conversation) {
    if (!conversation) {
        return;
    }
    mybot_agora_rtc_fini();
    memset(&conversation->cbs, 0, sizeof(conversation->cbs));
    conversation->app_id[0] = '\0';
    conversation->channel[0] = '\0';
    conversation->token[0] = '\0';
    conversation->uid[0] = '\0';
}
