#include <mybot/mybot.h>
#include <mybot/platform/mybot_audio.h>

int main(void) {
    const mybot_audio_capture_ops_t *capture = mybot_audio_device_get_capture();
    return capture == 0 && mybot_app_get_state() == MYBOT_APP_STATE_STOPPED ? 0 : 1;
}
