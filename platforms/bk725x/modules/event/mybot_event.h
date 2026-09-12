/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_EVENT_H_
#define MYBOT_EVENT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MYBOT_EVENT_BUTTON_VOLUME_UP = 0,
    MYBOT_EVENT_BUTTON_VOLUME_DOWN,
    MYBOT_EVENT_BUTTON_CONVERSATION_TOGGLE,
    MYBOT_EVENT_BUTTON_PROVISIONING_REQUEST,
    MYBOT_EVENT_TYPE_COUNT,
} mybot_event_type_t;

typedef struct {
    mybot_event_type_t type;
} mybot_event_t;

/* Initializes the process-wide event queue. */
int mybot_event_init(void);

/* All event producers and consumers must be stopped before deinitialization. */
void mybot_event_deinit(void);

/* Posts from thread context without blocking the producer. */
int mybot_event_post(mybot_event_type_t type);


/* Waits for the next event. timeout_ms follows the BK RTOS timeout convention. */
int mybot_event_wait(mybot_event_t *event, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_EVENT_H_ */
