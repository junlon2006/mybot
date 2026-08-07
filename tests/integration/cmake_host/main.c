#include <mybot/mybot.h>
#include <mybot/mybot_build_config.h>
#include <mybot/mybot_version.h>
#include <mybot/platform/mybot_audio.h>

#include <string.h>

int main(void) {
    const mybot_audio_capture_ops_t *capture = mybot_audio_device_get_capture();
    return capture == 0 && mybot_app_get_state() == MYBOT_APP_STATE_STOPPED &&
                   strcmp(mybot_version_string(), MYBOT_VERSION_STRING) == 0 &&
                   MYBOT_AUDIO_PTIME_MS == 60
               ? 0
               : 1;
}
