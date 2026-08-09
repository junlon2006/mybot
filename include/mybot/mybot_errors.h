/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_ERRORS_H_
#define MYBOT_ERRORS_H_

/*
 * Unified result-code convention for the mybot public API.
 *
 *   0       success
 *   > 0     success with a meaningful positive payload
 *           (bytes/frames transferred, HTTP status code, poll interval, ...)
 *   < 0     failure; the named codes below distinguish common cases.
 *           MYBOT_ERR_FAIL is the generic failure any API may return.
 *
 * A negative code never represents a successful outcome: for example
 * MYBOT_ERR_NOT_FOUND from the platform KV-store get callback means the key is
 * absent, which callers may treat as a benign first-boot state.
 */

#define MYBOT_OK 0
#define MYBOT_ERR_FAIL (-1)
#define MYBOT_ERR_INVALID_ARG (-2)
#define MYBOT_ERR_NOT_FOUND (-3)
#define MYBOT_ERR_UNAVAILABLE (-4)
#define MYBOT_ERR_NOMEM (-5)
#define MYBOT_ERR_TIMEOUT (-6)
#define MYBOT_ERR_BUSY (-7)

#endif /* MYBOT_ERRORS_H_ */
