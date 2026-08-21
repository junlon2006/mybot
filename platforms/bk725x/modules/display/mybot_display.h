/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_DISPLAY_H_
#define MYBOT_DISPLAY_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MYBOT_DISPLAY_SCREEN_STARTING = 0,
    MYBOT_DISPLAY_SCREEN_WIFI_PROVISIONING,
    MYBOT_DISPLAY_SCREEN_WIFI_DISCONNECTED,
    MYBOT_DISPLAY_SCREEN_STARTING_SERVICES,
    MYBOT_DISPLAY_SCREEN_PAIRING,
    MYBOT_DISPLAY_SCREEN_READY,
    MYBOT_DISPLAY_SCREEN_IN_CONVERSATION,
    MYBOT_DISPLAY_SCREEN_FAILED,
    MYBOT_DISPLAY_SCREEN_STOPPING,
    MYBOT_DISPLAY_SCREEN_COUNT,
} mybot_display_screen_t;

/* Owns both GC9D01 panels and keeps them alive across mybot restarts. */
int mybot_display_init(void);
void mybot_display_deinit(void);
bool mybot_display_is_ready(void);

/* Calls are serialized through the display worker. */
int mybot_display_show_screen(mybot_display_screen_t screen);

/* The code must contain exactly six ASCII digits. */
int mybot_display_show_pair_code(const char *pair_code);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_DISPLAY_H_ */
