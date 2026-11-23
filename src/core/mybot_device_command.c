/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_device_command.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

/* DeviceCommand objects are small and fixed: one pass rejects unknown or repeated keys. */
static bool read_fields(const mybot_json_t *object, const char *const *names, size_t count,
                        unsigned required, const mybot_json_t **fields) {
    if (!object || object->type != MYBOT_JSON_OBJECT) {
        return false;
    }
    memset(fields, 0, count * sizeof(const mybot_json_t *));
    unsigned seen = 0;
    for (const mybot_json_t *item = object->child; item; item = item->next) {
        size_t index = 0;
        while (index < count && (!item->string || strcmp(item->string, names[index]) != 0)) {
            index++;
        }
        if (index == count || (seen & (1u << index))) {
            return false;
        }
        seen |= 1u << index;
        fields[index] = item;
    }
    return (seen & required) == required;
}

static bool is_string(const mybot_json_t *value, const char *expected) {
    const char *text = mybot_json_get_string(value);
    return text && strcmp(text, expected) == 0;
}

static bool read_id(const mybot_json_t *value, char *destination) {
    const char *text = mybot_json_get_string(value);
    if (!text) {
        return false;
    }
    size_t length = strlen(text);
    if (length == 0 || length >= MYBOT_DEVICE_COMMAND_ID_CAPACITY) {
        return false;
    }
    if (destination) {
        memcpy(destination, text, length + 1);
    }
    return true;
}

static bool read_integer(const mybot_json_t *value, int minimum, int maximum, int *result) {
    if (!value || value->type != MYBOT_JSON_NUMBER || !isfinite(value->valuedouble) ||
        value->valuedouble < minimum || value->valuedouble > maximum ||
        value->valuedouble != (double)value->valueint) {
        return false;
    }
    *result = (int)value->valueint;
    return true;
}

bool mybot_device_command_parse(const mybot_json_t *root, mybot_device_command_t *command) {
    static const char *const names[] = {"type",       "schema",     "version",
                                        "message_id", "command_id", "conversation_id",
                                        "command",    "issued_at",  "expires_at"};
    const mybot_json_t *fields[9];
    int version;
    if (!command || !read_fields(root, names, 9, (1u << 7) - 1, fields) ||
        !is_string(fields[0], "device_command") ||
        !is_string(fields[1], "botstation.device-command") ||
        !read_integer(fields[2], 1, 1, &version)) {
        return false;
    }
    memset(command, 0, sizeof(*command));
    if (!read_id(fields[3], NULL) || !read_id(fields[4], command->command_id) ||
        !read_id(fields[5], command->conversation_id)) {
        return false;
    }
    static const char *const command_names[] = {"type", "name", "params"};
    const mybot_json_t *command_fields[3];
    if (!read_fields(fields[6], command_names, 3, 7, command_fields) ||
        !is_string(command_fields[0], "action")) {
        return false;
    }
    const char *parameter_name;
    int minimum;
    if (is_string(command_fields[1], "audio.set_volume")) {
        command->type = MYBOT_DEVICE_COMMAND_SET_VOLUME;
        parameter_name = "level";
        minimum = 0;
    } else if (is_string(command_fields[1], "audio.adjust_volume")) {
        command->type = MYBOT_DEVICE_COMMAND_ADJUST_VOLUME;
        parameter_name = "delta";
        minimum = -100;
    } else {
        return false;
    }
    const char *const parameter_names[] = {parameter_name};
    const mybot_json_t *parameters[1];
    return read_fields(command_fields[2], parameter_names, 1, 1, parameters) &&
           read_integer(parameters[0], minimum, 100, &command->value);
}
