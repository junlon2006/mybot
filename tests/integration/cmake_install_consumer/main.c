/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/mybot.h>
#include <mybot/mybot_build_config.h>
#include <mybot/mybot_version.h>

#include <string.h>

int main(void) {
    return mybot_get_state() == MYBOT_STATE_STOPPED &&
                   strcmp(mybot_version_string(), MYBOT_VERSION_STRING) == 0 &&
                   MYBOT_AUDIO_PTIME_MS == 60
               ? 0
               : 1;
}
