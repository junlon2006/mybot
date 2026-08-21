/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_LANGUAGE_H_
#define MYBOT_LANGUAGE_H_

/* Platform-level language configuration. The device language is selected in
 * the mybot SDK Kconfig via CONFIG_MYBOT_LANGUAGE_ZH_CN or
 * CONFIG_MYBOT_LANGUAGE_EN_US; everything language-dependent below follows
 * from that single choice. */

#ifdef CONFIG_MYBOT_LANGUAGE_ZH_CN
#define MYBOT_SERVER_BASE "https://mybot.sh2.agoralab.co/api"
#define MYBOT_ASSETS_DIR "mybot/assets"
#define MYBOT_LANGUAGE_TAG "zh-CN"
#elif defined(CONFIG_MYBOT_LANGUAGE_EN_US)
#define MYBOT_SERVER_BASE "https://mybot.sg3.agoralab.co/api"
#define MYBOT_ASSETS_DIR "mybot/assets"
#define MYBOT_LANGUAGE_TAG "en-US"
#else
#error "no mybot language selected: enable CONFIG_MYBOT_LANGUAGE_ZH_CN or \
CONFIG_MYBOT_LANGUAGE_EN_US"
#endif

#endif /* MYBOT_LANGUAGE_H_ */
