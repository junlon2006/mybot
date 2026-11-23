/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_DEVICE_COMMAND_H_
#define MYBOT_DEVICE_COMMAND_H_

#include "mybot_json.h"

#include <stdbool.h>

#define MYBOT_DEVICE_COMMAND_ID_CAPACITY 128

typedef enum {
    MYBOT_DEVICE_COMMAND_SET_VOLUME,
    MYBOT_DEVICE_COMMAND_ADJUST_VOLUME,
} mybot_device_command_type_t;

typedef struct {
    mybot_device_command_type_t type;
    char command_id[MYBOT_DEVICE_COMMAND_ID_CAPACITY];
    char conversation_id[MYBOT_DEVICE_COMMAND_ID_CAPACITY];
    int value;
} mybot_device_command_t;

/* Parse inner DeviceCommand v1 audio.set_volume/audio.adjust_volume actions without execution
 * or session access. value is an absolute level (0..100) or relative delta (-100..100).
 * issued_at and expires_at are optional and ignored.
 * root is borrowed; command owns its fields and is valid only on success. */
bool mybot_device_command_parse(const mybot_json_t *root, mybot_device_command_t *command);

#endif /* MYBOT_DEVICE_COMMAND_H_ */
