/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_APP_INTERNAL_H_
#define MYBOT_APP_INTERNAL_H_

/* Internal commands operate on an explicit runtime instance. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mybot_runtime mybot_runtime_t;

void mybot_app_start_conversation(mybot_runtime_t *runtime);

void mybot_app_stop_conversation(mybot_runtime_t *runtime);

void mybot_app_pair(mybot_runtime_t *runtime);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_APP_INTERNAL_H_ */
