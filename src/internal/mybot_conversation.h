/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_CONVERSATION_H_
#define MYBOT_CONVERSATION_H_

#include "mybot_device_lifecycle.h"
#include "mybot_agora_rtc.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*on_remote_audio)(uint32_t uid, const void *data, size_t len, void *user_data);
    void (*on_state_changed)(mybot_rtc_state_t state, void *user_data);
    void (*on_token_will_expire)(void *user_data);
    void *user_data;
} mybot_conversation_callbacks_t;

typedef struct {
    mybot_conversation_callbacks_t cbs;
    char app_id[64];
    char channel[128];
    char token[MYBOT_DEVICE_CLIENT_MAX_TOKEN];
    char uid[64];
} mybot_conversation_t;

int mybot_conversation_start(mybot_conversation_t *conversation,
                             const mybot_conversation_params_t *params,
                             const mybot_conversation_callbacks_t *callbacks);
int mybot_conversation_stop(mybot_conversation_t *conversation);
int mybot_conversation_send_audio(mybot_conversation_t *conversation, const void *data, size_t len);
int mybot_conversation_renew_token(mybot_conversation_t *conversation, const char *token);
void mybot_conversation_fini(mybot_conversation_t *conversation);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_CONVERSATION_H_ */
