/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_VERSION_H_
#define MYBOT_VERSION_H_

#define MYBOT_VERSION_MAJOR 0
#define MYBOT_VERSION_MINOR 1
#define MYBOT_VERSION_PATCH 0
#define MYBOT_VERSION_PRERELEASE "rc.1"
#define MYBOT_VERSION_STRING "0.1.0-rc.1"

#ifdef __cplusplus
extern "C" {
#endif

/** Return the SDK semantic version string. */
const char *mybot_version_string(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_VERSION_H_ */
