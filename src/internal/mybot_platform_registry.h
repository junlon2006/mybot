/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_PLATFORM_REGISTRY_H_
#define MYBOT_PLATFORM_REGISTRY_H_

#include <mybot/platform/mybot_platform.h>

#include <stdbool.h>

bool mybot_platform_registry_is_registered(void);

const mybot_platform_descriptor_t *mybot_platform_registry_get(void);

#endif /* MYBOT_PLATFORM_REGISTRY_H_ */
