/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_KEY_DISPATCHER_H_
#define MYBOT_KEY_DISPATCHER_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MYBOT_KEY_ACTION_VOLUME_UP = 0,
    MYBOT_KEY_ACTION_VOLUME_DOWN,
    MYBOT_KEY_ACTION_CONVERSATION_START,
    MYBOT_KEY_ACTION_CONVERSATION_STOP,
} mybot_key_action_t;

typedef void (*mybot_key_action_handler_t)(mybot_key_action_t action, void *user_data);

int mybot_key_dispatcher_prepare(void);
int mybot_key_dispatcher_subscribe(void **subscription, mybot_key_action_handler_t handler,
                                   void *user_data);
int mybot_key_dispatcher_unsubscribe(void *subscription);
int mybot_key_dispatcher_publish(mybot_key_action_t action);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_KEY_DISPATCHER_H_ */
