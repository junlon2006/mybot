/* SPDX-License-Identifier: Apache-2.0 */
#include "bk_private/bk_init.h"
#include <media_service.h>

#include <mybot_controller.h>

int main(void) {
    bk_init();
    media_service_init();
    return mybot_controller_start();
}
