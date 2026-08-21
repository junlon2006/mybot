/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_PLATFORM_LOG_H_
#define MYBOT_PLATFORM_LOG_H_

#include <api/aosl_log.h>

#define MYBOT_PLATFORM_LOG(level, tag, format, ...)                                      \
    aosl_log((level), "[%d][aosl][%s][%s:%u]" format "\n", (level), (tag), __FUNCTION__, \
             __LINE__, ##__VA_ARGS__)

#define MYBOT_LOGD(tag, format, ...)                                                      \
    MYBOT_PLATFORM_LOG(AOSL_LOG_DEBUG, tag, format, ##__VA_ARGS__)
#define MYBOT_LOGI(tag, format, ...)                                                      \
    MYBOT_PLATFORM_LOG(AOSL_LOG_NOTICE, tag, format, ##__VA_ARGS__)
#define MYBOT_LOGW(tag, format, ...)                                                      \
    MYBOT_PLATFORM_LOG(AOSL_LOG_WARNING, tag, format, ##__VA_ARGS__)
#define MYBOT_LOGE(tag, format, ...)                                                      \
    MYBOT_PLATFORM_LOG(AOSL_LOG_ERROR, tag, format, ##__VA_ARGS__)

#endif /* MYBOT_PLATFORM_LOG_H_ */
