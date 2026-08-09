/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_HTTPS_INTERNAL_H_
#define MYBOT_HTTPS_INTERNAL_H_

#include <mybot/platform/mybot_https.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * SDK-internal HTTPS registry query. The public
 * mybot/platform/mybot_https.h only exposes the TLS transport contract
 * (ops table + mybot_https_register()); the SDK core checks registration
 * during startup.
 */

/** Return whether a platform TLS transport has been registered. */
bool mybot_https_is_registered(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_HTTPS_INTERNAL_H_ */
