/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_ERRORS_H_
#define MYBOT_ERRORS_H_

/* A negative result from the KV-store get callback means the key is absent and
 * may be treated as a benign first-boot state. */
#define MYBOT_ERR_NOT_FOUND (-3)

#endif /* MYBOT_ERRORS_H_ */
