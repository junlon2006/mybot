/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/mybot.h>
#include <mybot/mybot_version.h>

#include <cstring>

int main() {
    return mybot_get_state() == MYBOT_STATE_STOPPED &&
                   std::strcmp(mybot_version_string(), MYBOT_VERSION_STRING) == 0
               ? 0
               : 1;
}
