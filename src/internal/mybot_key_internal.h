/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_KEY_INTERNAL_H_
#define MYBOT_KEY_INTERNAL_H_

#include <mybot/platform/mybot_key.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * SDK-internal key facade. The public mybot/platform/mybot_key.h only exposes
 * the platform contract (event enum, handler typedef, ops table and
 * mybot_key_register()); the SDK core manages the key implementation lifecycle.
 */

/** Initialize the registered implementation and install the application event handler. */
int mybot_key_init(mybot_key_event_handler_t handler, void *user_data);

/** Stop the implementation and release its resources. No events are emitted after return. */
void mybot_key_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_KEY_INTERNAL_H_ */
