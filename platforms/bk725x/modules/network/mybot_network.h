/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_NETWORK_H_
#define MYBOT_NETWORK_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reports whether at least one valid STA credential is persisted. */
int mybot_network_is_configured(bool *configured);

/* Starts the normal STA worker. */
int mybot_network_start(void);

/* Stops STA, unregisters callbacks, and joins the worker. Idempotent. */
int mybot_network_stop(void);

bool mybot_network_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_NETWORK_H_ */
