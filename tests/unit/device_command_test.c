/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_device_command.h"

#include <api/aosl.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VALID_ACTION "{\"type\":\"action\",\"name\":\"audio.set_volume\",\"params\":{\"level\":50}}"
#define VALID_ADJUST                                                                               \
    "{\"type\":\"action\",\"name\":\"audio.adjust_volume\",\"params\":{\"delta\":-10}}"

static mybot_json_t *fixture(const char *command, const char *extra) {
    char text[2048];
    int length = snprintf(text, sizeof(text),
                          "{\"type\":\"device_command\",\"schema\":\"botstation.device-command\","
                          "\"version\":1,\"message_id\":\"msg-1\",\"command_id\":\"cmd-1\","
                          "\"conversation_id\":\"conv-1\","
                          "\"command\":%s%s}",
                          command, extra);
    assert(length > 0 && (size_t)length < sizeof(text));
    mybot_json_t *root = mybot_json_parse(text);
    assert(root != NULL);
    return root;
}

static bool parse(const char *command, mybot_device_command_t *result) {
    mybot_json_t *root = fixture(command, "");
    bool valid = mybot_device_command_parse(root, result);
    mybot_json_delete(root);
    return valid;
}

static void test_valid_commands(void) {
    mybot_device_command_t command;
    assert(parse(VALID_ACTION, &command));
    assert(command.type == MYBOT_DEVICE_COMMAND_SET_VOLUME && command.value == 50);
    assert(strcmp(command.command_id, "cmd-1") == 0);
    assert(strcmp(command.conversation_id, "conv-1") == 0);
    assert(parse(VALID_ADJUST, &command));
    assert(command.type == MYBOT_DEVICE_COMMAND_ADJUST_VOLUME && command.value == -10);
    assert(strcmp(command.command_id, "cmd-1") == 0);
    assert(strcmp(command.conversation_id, "conv-1") == 0);
}

static void test_parameter_boundaries(void) {
    static const struct {
        const char *value;
        bool valid;
    } cases[] = {
        {"0", true},
        {"100", true},
        {"50.0", true},
        {"5e1", true},
        {"-1", false},
        {"101", false},
        {"0.5", false},
        {"99.9", false},
        {"\"50\"", false},
        {"true", false},
        {"false", false},
        {"null", false},
        {"{}", false},
        {"[]", false},
        {"9223372036854775807", false},
        {"-9223372036854775808", false},
    };

    mybot_device_command_t result;
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        char command[192];
        int length = snprintf(command, sizeof(command),
                              "{\"type\":\"action\",\"name\":\"audio.set_volume\","
                              "\"params\":{\"level\":%s}}",
                              cases[index].value);
        assert(length > 0 && (size_t)length < sizeof(command));
        assert(parse(command, &result) == cases[index].valid);
    }
}

static void test_relative_parameter_boundaries(void) {
    static const struct {
        const char *value;
        bool valid;
        int expected;
    } cases[] = {
        {"-10", true, -10},
        {"10", true, 10},
        {"0", true, 0},
        {"-100", true, -100},
        {"100", true, 100},
        {"-10.0", true, -10},
        {"-1e1", true, -10},
        {"-101", false, 0},
        {"101", false, 0},
        {"-0.5", false, 0},
        {"0.5", false, 0},
        {"\"-10\"", false, 0},
        {"true", false, 0},
        {"false", false, 0},
        {"null", false, 0},
        {"{}", false, 0},
        {"[]", false, 0},
        {"9223372036854775807", false, 0},
        {"-9223372036854775808", false, 0},
    };

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        char action[192];
        int length = snprintf(action, sizeof(action),
                              "{\"type\":\"action\",\"name\":\"audio.adjust_volume\","
                              "\"params\":{\"delta\":%s}}",
                              cases[index].value);
        assert(length > 0 && (size_t)length < sizeof(action));
        mybot_device_command_t command;
        assert(parse(action, &command) == cases[index].valid);
        if (cases[index].valid) {
            assert(command.type == MYBOT_DEVICE_COMMAND_ADJUST_VOLUME);
            assert(command.value == cases[index].expected);
        }
    }
}

static void test_invalid_commands(void) {
    static const char *const cases[] = {
        "{}",
        "[]",
        "{\"type\":\"action\",\"name\":\"audio.unknown\",\"params\":{}}",
        "{\"type\":\"action\",\"name\":\"audio.set_mute\",\"params\":{\"muted\":true}}",
        "{\"type\":\"ACTION\",\"name\":\"audio.set_volume\",\"params\":{\"level\":50}}",
        "{\"type\":\"action\",\"name\":\"Audio.set_volume\",\"params\":{\"level\":50}}",
        "{\"type\":\"action\",\"name\":\"audio.set_volume\",\"params\":{}}",
        "{\"type\":\"action\",\"name\":\"audio.set_volume\",\"params\":[]} ",
        "{\"type\":\"action\",\"name\":\"audio.set_volume\",\"params\":null}",
        "{\"type\":\"action\",\"name\":\"audio.set_volume\",\"params\":{\"Level\":50}}",
        "{\"type\":\"action\",\"name\":\"audio.set_volume\",\"params\":{\"level\":50,\"extra\":0}}",
        "{\"type\":\"action\",\"name\":\"audio.set_volume\",\"params\":{\"level\":1,\"level\":2}}",
        ("{\"type\":\"action\",\"type\":\"action\",\"name\":\"audio.set_volume\",\"params\":{"
         "\"level\":50}}"),
        "{\"type\":\"action\",\"name\":\"audio.set_volume\",\"params\":{\"level\":50},\"extra\":0}",
        "{\"type\":\"action\",\"name\":\"audio.adjust_volume\",\"params\":{}}",
        "{\"type\":\"action\",\"name\":\"audio.adjust_volume\",\"params\":[]}",
        "{\"type\":\"action\",\"name\":\"audio.adjust_volume\",\"params\":null}",
        "{\"type\":\"action\",\"name\":\"audio.adjust_volume\",\"params\":{\"level\":10}}",
        "{\"type\":\"action\",\"name\":\"audio.set_volume\",\"params\":{\"delta\":10}}",
        "{\"type\":\"action\",\"name\":\"audio.adjust_volume\",\"params\":{\"Delta\":10}}",
        "{\"type\":\"action\",\"name\":\"Audio.adjust_volume\",\"params\":{\"delta\":10}}",
        ("{\"type\":\"action\",\"name\":\"audio.adjust_volume\","
         "\"params\":{\"delta\":10,\"delta\":20}}"),
        ("{\"type\":\"action\",\"name\":\"audio.adjust_volume\","
         "\"params\":{\"delta\":10,\"extra\":0}}"),
        "{\"type\":\"scene\",\"name\":\"emotion.happy\",\"params\":{}}",
        "{\"type\":\"scene\",\"name\":\"audio.set_volume\",\"params\":{}}",
        "{\"type\":\"action\",\"name\":\"emotion.happy\",\"params\":{}}",
    };
    mybot_device_command_t result;
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        assert(!parse(cases[index], &result));
    }
}

static void test_envelope(void) {
    mybot_device_command_t result;
    assert(!mybot_device_command_parse(NULL, &result));
    assert(!mybot_device_command_parse(NULL, NULL));
    mybot_json_t *root = fixture(VALID_ACTION, "");
    assert(!mybot_device_command_parse(root, NULL));
    mybot_json_delete(root);

    static const char *const extras[] = {",\"version\":1", ",\"extra\":0", ",\"Version\":1"};
    for (size_t index = 0; index < sizeof(extras) / sizeof(extras[0]); index++) {
        root = fixture(VALID_ACTION, extras[index]);
        assert(!mybot_device_command_parse(root, &result));
        mybot_json_delete(root);
    }
    root = mybot_json_parse("{\"type\":\"device_command\"}");
    assert(root != NULL && !mybot_device_command_parse(root, &result));
    mybot_json_delete(root);
    root = mybot_json_parse("[]");
    assert(root != NULL && !mybot_device_command_parse(root, &result));
    mybot_json_delete(root);

    static const char *const required[] = {"type",       "schema",          "version", "message_id",
                                           "command_id", "conversation_id", "command"};
    for (size_t index = 0; index < sizeof(required) / sizeof(required[0]); index++) {
        root = fixture(VALID_ACTION, "");
        mybot_json_t **link = &root->child;
        while (*link && strcmp((*link)->string, required[index]) != 0) {
            link = &(*link)->next;
        }
        assert(*link != NULL);
        mybot_json_t *field = *link;
        *link = field->next;
        assert(!mybot_device_command_parse(root, &result));
        *link = field;
        mybot_json_delete(root);
    }

    static const char *const strings[] = {"type", "schema", "message_id", "command_id",
                                          "conversation_id"};
    for (size_t index = 0; index < sizeof(strings) / sizeof(strings[0]); index++) {
        root = fixture(VALID_ACTION, "");
        mybot_json_t *field = mybot_json_get_object_item(root, strings[index]);
        assert(field != NULL);
        field->type = MYBOT_JSON_NULL;
        assert(!mybot_device_command_parse(root, &result));
        field->type = MYBOT_JSON_STRING;
        field->valuestring[0] = '\0';
        assert(!mybot_device_command_parse(root, &result));
        mybot_json_delete(root);
    }
    static const double versions[] = {0, 2, 1.5, NAN, INFINITY};
    for (size_t index = 0; index < sizeof(versions) / sizeof(versions[0]); index++) {
        root = fixture(VALID_ACTION, "");
        mybot_json_t *version = mybot_json_get_object_item(root, "version");
        version->valuedouble = versions[index];
        assert(!mybot_device_command_parse(root, &result));
        version->type = MYBOT_JSON_STRING;
        assert(!mybot_device_command_parse(root, &result));
        mybot_json_delete(root);
    }
}

static void test_id_bounds(void) {
    mybot_device_command_t result;
    static const char *const names[] = {"message_id", "command_id", "conversation_id"};
    char id[MYBOT_DEVICE_COMMAND_ID_CAPACITY + 1];
    memset(id, 'x', sizeof(id) - 1);
    id[sizeof(id) - 1] = '\0';
    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
        for (size_t length = MYBOT_DEVICE_COMMAND_ID_CAPACITY - 1;
             length <= MYBOT_DEVICE_COMMAND_ID_CAPACITY; length++) {
            id[length] = '\0';
            mybot_json_t *root = fixture(VALID_ACTION, "");
            mybot_json_t *field = mybot_json_get_object_item(root, names[index]);
            char *original = field->valuestring;
            field->valuestring = id;
            assert(mybot_device_command_parse(root, &result) ==
                   (length < MYBOT_DEVICE_COMMAND_ID_CAPACITY));
            field->valuestring = original;
            mybot_json_delete(root);
            id[length] = 'x';
        }
    }
}

static void test_ignored_timestamps(void) {
    static const char *const timestamps[] = {
        "",
        ",\"issued_at\":\"1970-01-01T00:00:00Z\",\"expires_at\":\"1970-01-01T00:00:01Z\"",
        ",\"issued_at\":\"9999-12-31T23:59:58Z\",\"expires_at\":\"9999-12-31T23:59:59Z\"",
        ",\"issued_at\":\"2026-09-21T10:00:10Z\",\"expires_at\":\"2026-09-21T10:00:00Z\"",
        ",\"issued_at\":\"not a date\",\"expires_at\":\"\"",
        ",\"issued_at\":null",
        ",\"expires_at\":false",
        ",\"issued_at\":123,\"expires_at\":{}",
        ",\"issued_at\":[],\"expires_at\":true",
    };
    mybot_device_command_t command;
    for (size_t index = 0; index < sizeof(timestamps) / sizeof(timestamps[0]); index++) {
        mybot_json_t *root = fixture(VALID_ACTION, timestamps[index]);
        assert(mybot_device_command_parse(root, &command));
        assert(command.type == MYBOT_DEVICE_COMMAND_SET_VOLUME && command.value == 50);
        mybot_json_delete(root);
    }
    static const char *const repeated[] = {
        ",\"issued_at\":null,\"issued_at\":null",
        ",\"expires_at\":null,\"expires_at\":null",
    };
    for (size_t index = 0; index < sizeof(repeated) / sizeof(repeated[0]); index++) {
        mybot_json_t *root = fixture(VALID_ACTION, repeated[index]);
        assert(!mybot_device_command_parse(root, &command));
        mybot_json_delete(root);
    }
}

static size_t s_allocation_count;
static size_t s_live_allocations;
static bool s_fail_allocation;

static void *test_malloc(size_t size) {
    s_allocation_count++;
    if (s_fail_allocation) {
        return NULL;
    }
    void *ptr = malloc(size);
    if (ptr) {
        s_live_allocations++;
    }
    return ptr;
}

static void test_free(void *ptr) {
    if (ptr) {
        assert(s_live_allocations > 0);
        s_live_allocations--;
    }
    free(ptr);
}

static void test_allocation_and_ownership(void) {
    const mybot_json_hooks_t hooks = {.malloc_fn = test_malloc, .free_fn = test_free};
    assert(mybot_json_init_hooks(&hooks) == 0);

    static const struct {
        const char *action;
        mybot_device_command_type_t type;
        int value;
    } commands[] = {{VALID_ACTION, MYBOT_DEVICE_COMMAND_SET_VOLUME, 50},
                    {VALID_ADJUST, MYBOT_DEVICE_COMMAND_ADJUST_VOLUME, -10}};

    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        mybot_json_t *root = fixture(commands[index].action, "");
        size_t allocations_before = s_allocation_count;
        s_fail_allocation = true;
        mybot_device_command_t command;
        assert(mybot_device_command_parse(root, &command));
        assert(s_allocation_count == allocations_before);
        mybot_json_delete(root);
        assert(s_live_allocations == 0);
        assert(strcmp(command.command_id, "cmd-1") == 0);
        assert(strcmp(command.conversation_id, "conv-1") == 0);
        assert(command.type == commands[index].type && command.value == commands[index].value);
        s_fail_allocation = false;
    }
    assert(mybot_json_init_hooks(NULL) == 0);
}

int main(void) {
    aosl_ctor();
    test_valid_commands();
    test_parameter_boundaries();
    test_relative_parameter_boundaries();
    test_invalid_commands();
    test_envelope();
    test_id_bounds();
    test_ignored_timestamps();
    test_allocation_and_ownership();
    aosl_dtor();
    puts("device_command_test: ok");
    return 0;
}
