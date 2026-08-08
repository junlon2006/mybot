/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/mybot.h>
#include <mybot/mybot_build_config.h>
#include <mybot/mybot_version.h>
#include <mybot/platform/mybot_audio.h>
#include <mybot/platform/mybot_key.h>
#include <mybot/platform/mybot_kv_store.h>
#include <mybot/platform/mybot_lcd.h>
#include <mybot/platform/mybot_https.h>
#include <mybot/platform/mybot_wake_words.h>
#include <mybot/platform/mybot_wifi.h>

#include <assert.h>
#include <string.h>

int main(void) {
    mybot_app_config_t config;
    memset(&config, 0, sizeof(config));

    assert(mybot_app_start(NULL) < 0);
    assert(mybot_app_start(&config) < 0);

    strcpy(config.server_base, "https://example.invalid");
    strcpy(config.device_id, "device-1");
    assert(mybot_app_start(&config) < 0);

    memset(config.server_base, 'x', sizeof(config.server_base));
    assert(mybot_app_start(&config) < 0);

    assert(strcmp(mybot_version_string(), MYBOT_VERSION_STRING) == 0);
    return MYBOT_VERSION_MAJOR == 0 ? 0 : 1;
}
