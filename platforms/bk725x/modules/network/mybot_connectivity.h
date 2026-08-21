/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_CONNECTIVITY_H_
#define MYBOT_CONNECTIVITY_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MYBOT_CONNECTIVITY_CONNECTED = 0,
    MYBOT_CONNECTIVITY_DISCONNECTED,
    MYBOT_CONNECTIVITY_FAILED,
} mybot_connectivity_event_t;

typedef void (*mybot_connectivity_handler_t)(mybot_connectivity_event_t event,
                                             void *user_data);

int mybot_connectivity_prepare(void);
int mybot_connectivity_subscribe(void **subscription, mybot_connectivity_handler_t handler,
                                 void *user_data);
int mybot_connectivity_unsubscribe(void *subscription);
int mybot_connectivity_publish(mybot_connectivity_event_t event);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_CONNECTIVITY_H_ */
