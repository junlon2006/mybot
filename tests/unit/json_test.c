/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_json.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t s_allocation_attempts;
static size_t s_successful_allocations;
static size_t s_free_calls;
static size_t s_fail_on_attempt = (size_t)-1;

static void *test_malloc(size_t size) {
    if (s_allocation_attempts++ == s_fail_on_attempt)
        return NULL;

    void *memory = malloc(size);
    if (memory)
        s_successful_allocations++;
    return memory;
}

static void test_free(void *memory) {
    if (!memory)
        return;
    s_free_calls++;
    free(memory);
}

static void reset_allocator_state(void) {
    s_allocation_attempts = 0;
    s_successful_allocations = 0;
    s_free_calls = 0;
    s_fail_on_attempt = (size_t)-1;
}

static void test_round_trip(void) {
    mybot_json_t *root = mybot_json_create_object();
    mybot_json_t *details = mybot_json_create_object();
    assert(root != NULL);
    assert(details != NULL);

    int result = mybot_json_add_string(root, "name", "mybot");
    assert(result == 0);
    result = mybot_json_add_number(root, "count", 42);
    assert(result == 0);
    result = mybot_json_add_bool(root, "enabled", true);
    assert(result == 0);
    result = mybot_json_add_string(details, "implementation", "namespaced-cjson");
    assert(result == 0);
    result = mybot_json_add_item(root, "details", details);
    assert(result == 0);

    char *serialized = mybot_json_print_unformatted(root);
    assert(serialized != NULL);
    assert(strstr(serialized, "\"name\":\"mybot\"") != NULL);
    mybot_json_delete(root);

    root = mybot_json_parse(serialized);
    mybot_json_free_string(serialized);
    assert(root != NULL);

    const char *name = mybot_json_get_string(mybot_json_get_object_item(root, "name"));
    assert(name != NULL);
    assert(strcmp(name, "mybot") == 0);

    int64_t count = 0;
    assert(mybot_json_get_integer(mybot_json_get_object_item(root, "count"), &count));
    assert(count == 42);
    assert(mybot_json_get_string(mybot_json_get_object_item(root, "count")) == NULL);

    mybot_json_t *enabled = mybot_json_get_object_item(root, "enabled");
    assert(enabled != NULL);
    assert(enabled->type == MYBOT_JSON_TRUE);

    details = mybot_json_get_object_item(root, "details");
    const char *implementation =
        mybot_json_get_string(mybot_json_get_object_item(details, "implementation"));
    assert(implementation != NULL);
    assert(strcmp(implementation, "namespaced-cjson") == 0);

    mybot_json_delete(root);
}

static void test_allocator_hooks(void) {
    mybot_json_hooks_t incomplete_hooks = {
        .malloc_fn = test_malloc,
        .free_fn = NULL,
    };
    int result = mybot_json_init_hooks(&incomplete_hooks);
    assert(result == -1);

    mybot_json_hooks_t hooks = {
        .malloc_fn = test_malloc,
        .free_fn = test_free,
    };
    reset_allocator_state();
    result = mybot_json_init_hooks(&hooks);
    assert(result == 0);

    mybot_json_t *root = mybot_json_create_object();
    assert(root != NULL);
    result = mybot_json_add_string(root, "allocator", "custom");
    assert(result == 0);
    char *serialized = mybot_json_print_unformatted(root);
    assert(serialized != NULL);

    mybot_json_delete(root);
    mybot_json_free_string(serialized);
    assert(s_successful_allocations == s_free_calls);

    result = mybot_json_init_hooks(NULL);
    assert(result == 0);
}

static void test_add_item_key_allocation_failure(void) {
    mybot_json_hooks_t hooks = {
        .malloc_fn = test_malloc,
        .free_fn = test_free,
    };
    reset_allocator_state();
    int result = mybot_json_init_hooks(&hooks);
    assert(result == 0);

    mybot_json_t *root = mybot_json_create_object();
    mybot_json_t *child = mybot_json_create_object();
    assert(root != NULL);
    assert(child != NULL);

    s_fail_on_attempt = s_allocation_attempts;
    result = mybot_json_add_item(root, "child", child);
    assert(result == -1);
    assert(root->child == NULL);
    assert(child->string == NULL);

    mybot_json_delete(child);
    mybot_json_delete(root);
    assert(s_successful_allocations == s_free_calls);

    result = mybot_json_init_hooks(NULL);
    assert(result == 0);
}

static void test_string_value_allocation_failure(void) {
    mybot_json_hooks_t hooks = {
        .malloc_fn = test_malloc,
        .free_fn = test_free,
    };
    reset_allocator_state();
    int result = mybot_json_init_hooks(&hooks);
    assert(result == 0);

    s_fail_on_attempt = 1;
    mybot_json_t *value = mybot_json_create_string("value");
    assert(value == NULL);
    assert(s_successful_allocations == s_free_calls);

    result = mybot_json_init_hooks(NULL);
    assert(result == 0);
}

static void assert_printed(mybot_json_t *item, int formatted, const char *expected) {
    char *printed = formatted ? mybot_json_print(item) : mybot_json_print_unformatted(item);
    assert(printed != NULL);
    assert(strcmp(printed, expected) == 0);
    mybot_json_free_string(printed);
}

static void assert_number(const mybot_json_t *item, double expected) {
    assert(item != NULL);
    assert((item->type & 0xff) == MYBOT_JSON_NUMBER);
    assert(item->valuedouble == expected);
}

static void test_scalar_types_and_strings(void) {
    mybot_json_t *item = mybot_json_create_null();
    assert(item != NULL);
    assert(item->type == MYBOT_JSON_NULL);
    assert_printed(item, 0, "null");
    mybot_json_delete(item);

    item = mybot_json_create_true();
    assert(item != NULL);
    assert(item->type == MYBOT_JSON_TRUE);
    assert_printed(item, 0, "true");
    mybot_json_delete(item);

    item = mybot_json_create_false();
    assert(item != NULL);
    assert(item->type == MYBOT_JSON_FALSE);
    assert_printed(item, 0, "false");
    mybot_json_delete(item);

    item = mybot_json_create_bool(0);
    assert(item != NULL);
    assert(item->type == MYBOT_JSON_FALSE);
    mybot_json_delete(item);

    item = mybot_json_create_number(10000000000.0);
    assert_printed(item, 0, "10000000000");
    mybot_json_delete(item);
    item = mybot_json_create_number(0.0000001);
    assert_printed(item, 0, "1.000000e-07");
    mybot_json_delete(item);
    item = mybot_json_create_number(1.25);
    assert_printed(item, 0, "1.25");
    mybot_json_delete(item);

    item = mybot_json_create_string("\"\\\b\f\n\r\t\x01");
    assert(item != NULL);
    assert_printed(item, 0, "\"\\\"\\\\\\b\\f\\n\\r\\t\\u0001\"");
    mybot_json_delete(item);

    item = mybot_json_parse("\"A\\u0042\\u00a2\\u20AC\\ud83d\\ude00\\b\\f\\n\\r\\t\\/\\\\\\\"\"");
    assert(item != NULL);
    assert(strcmp(mybot_json_get_string(item),
                  "AB\xc2\xa2\xe2\x82\xac\xf0\x9f\x98\x80\b\f\n\r\t/\\\"") == 0);
    mybot_json_delete(item);

    assert(mybot_json_create_string(NULL) == NULL);
    assert(mybot_json_print(NULL) == NULL);
    assert(mybot_json_print_unformatted(NULL) == NULL);
    mybot_json_free_string(NULL);
}

static void test_parse_options_and_errors(void) {
    const char trailing[] = "true trailing";
    const char *end = NULL;
    mybot_json_t *item = mybot_json_parse_with_options(trailing, &end, 0);
    assert(item != NULL);
    assert(item->type == MYBOT_JSON_TRUE);
    assert(end == trailing + 4);
    assert(mybot_json_get_error_pointer() == NULL);
    mybot_json_delete(item);

    item = mybot_json_parse_with_options(trailing, &end, 1);
    assert(item == NULL);
    assert(mybot_json_get_error_pointer() == trailing + 5);

    const char terminated[] = " \tfalse\r\n";
    item = mybot_json_parse_with_options(terminated, &end, 1);
    assert(item != NULL);
    assert(item->type == MYBOT_JSON_FALSE);
    assert(*end == '\0');
    mybot_json_delete(item);

    const char malformed_object[] = "{\"a\" 1}";
    assert(mybot_json_parse(malformed_object) == NULL);
    assert(mybot_json_get_error_pointer() == malformed_object + 5);
    assert(mybot_json_parse("[1,]") == NULL);
    assert(mybot_json_parse("[1 2]") == NULL);
    assert(mybot_json_parse("{\"a\":1,}") == NULL);
    assert(mybot_json_parse("{\"a\":1,\"b\" 2}") == NULL);
    assert(mybot_json_parse("{\"a\":1,\"b\":}") == NULL);
    assert(mybot_json_parse("{\"a\":1 \"b\":2}") == NULL);
    assert(mybot_json_parse("not-json") == NULL);
    assert(mybot_json_parse(NULL) == NULL);

    item = mybot_json_parse("[1.25e+2,-2.5E-1,0,null,true,false,\"x\",[],{}]");
    assert(item != NULL);
    assert(mybot_json_get_array_size(item) == 9);
    assert_number(mybot_json_get_array_item(item, 0), 125.0);
    assert_number(mybot_json_get_array_item(item, 1), -0.25);
    assert(mybot_json_get_array_item(item, 3)->type == MYBOT_JSON_NULL);
    assert(mybot_json_get_array_item(item, 7)->type == MYBOT_JSON_ARRAY);
    assert(mybot_json_get_array_item(item, 8)->type == MYBOT_JSON_OBJECT);
    assert(mybot_json_get_array_item(item, 9) == NULL);
    mybot_json_delete(item);
}

static void test_array_builders(void) {
    static const int ints[] = {-2, 0, 7};
    static const float floats[] = {-1.5f, 2.5f};
    static const double doubles[] = {0.125, 10000000000.0};
    static const char *strings[] = {"first", "second", "third"};

    mybot_json_t *array = mybot_json_create_int_array(ints, 3);
    assert(array != NULL);
    assert(mybot_json_get_array_size(array) == 3);
    assert_number(mybot_json_get_array_item(array, 0), -2.0);
    assert_number(mybot_json_get_array_item(array, 2), 7.0);
    mybot_json_delete(array);

    array = mybot_json_create_float_array(floats, 2);
    assert(array != NULL);
    assert_number(mybot_json_get_array_item(array, 0), -1.5);
    assert_number(mybot_json_get_array_item(array, 1), 2.5);
    mybot_json_delete(array);

    array = mybot_json_create_double_array(doubles, 2);
    assert(array != NULL);
    assert_number(mybot_json_get_array_item(array, 0), 0.125);
    assert_number(mybot_json_get_array_item(array, 1), 10000000000.0);
    mybot_json_delete(array);

    array = mybot_json_create_string_array(strings, 3);
    assert(array != NULL);
    assert(strcmp(mybot_json_get_string(mybot_json_get_array_item(array, 1)), "second") == 0);
    mybot_json_delete(array);

    array = mybot_json_create_int_array(NULL, 0);
    assert(array != NULL);
    assert(mybot_json_get_array_size(array) == 0);
    assert_printed(array, 0, "[]");
    mybot_json_delete(array);
}

static void test_array_mutation(void) {
    mybot_json_t *array = mybot_json_create_array();
    assert(array != NULL);
    assert(mybot_json_add_item_to_array(array, NULL) == -1);

    mybot_json_t *one = mybot_json_create_number(1);
    mybot_json_t *two = mybot_json_create_number(2);
    mybot_json_t *three = mybot_json_create_number(3);
    assert(one != NULL && two != NULL && three != NULL);
    assert(mybot_json_add_item_to_array(NULL, one) == -1);
    assert(mybot_json_add_item_to_array(array, one) == 0);
    assert(mybot_json_add_item_to_array(array, two) == 0);
    assert(mybot_json_add_item_to_array(array, three) == 0);
    assert(mybot_json_get_array_size(array) == 3);
    assert(two->prev == one && two->next == three);

    mybot_json_t *replacement = mybot_json_create_number(20);
    mybot_json_replace_item_in_array(array, 1, replacement);
    assert(mybot_json_get_array_item(array, 1) == replacement);
    replacement = mybot_json_create_number(10);
    mybot_json_replace_item_in_array(array, 0, replacement);
    assert(array->child == replacement && replacement->prev == NULL);
    replacement = mybot_json_create_number(30);
    mybot_json_replace_item_in_array(array, 2, replacement);
    assert(mybot_json_get_array_item(array, 2) == replacement && replacement->next == NULL);

    mybot_json_t *unused = mybot_json_create_number(99);
    mybot_json_replace_item_in_array(array, 99, unused);
    assert(unused->prev == NULL && unused->next == NULL);
    mybot_json_delete(unused);

    mybot_json_t *detached = mybot_json_detach_item_from_array(array, 1);
    assert(detached != NULL && detached->prev == NULL && detached->next == NULL);
    mybot_json_delete(detached);
    detached = mybot_json_detach_item_from_array(array, 0);
    assert(detached != NULL && detached->prev == NULL && detached->next == NULL);
    mybot_json_delete(detached);
    assert(mybot_json_detach_item_from_array(array, 9) == NULL);
    mybot_json_delete_item_from_array(array, 0);
    assert(mybot_json_get_array_size(array) == 0);
    mybot_json_delete_item_from_array(array, 0);
    mybot_json_delete(array);
}

static void test_object_mutation_and_accessors(void) {
    mybot_json_t *object = mybot_json_create_object();
    assert(object != NULL);
    assert(mybot_json_add_null(object, "nothing") == 0);
    assert(mybot_json_add_string(object, "Name", "mybot") == 0);
    assert(mybot_json_add_number(object, "count", 42) == 0);
    assert(mybot_json_add_bool(object, "enabled", false) == 0);
    assert(mybot_json_get_array_size(object) == 4);
    assert(mybot_json_get_object_item(object, "name") ==
           mybot_json_get_object_item(object, "NAME"));
    assert(mybot_json_get_object_item(object, "missing") == NULL);
    assert(mybot_json_get_object_item(NULL, "name") == NULL);
    assert(mybot_json_get_object_item(object, NULL) == NULL);

    const mybot_json_t *name = mybot_json_get_object_item(object, "name");
    assert(strcmp(mybot_json_get_string(name), "mybot") == 0);
    assert(mybot_json_get_string(NULL) == NULL);
    assert(mybot_json_get_string(mybot_json_get_object_item(object, "count")) == NULL);
    int64_t integer = 0;
    assert(mybot_json_get_integer(mybot_json_get_object_item(object, "count"), &integer));
    assert(integer == 42);
    assert(!mybot_json_get_integer(NULL, &integer));
    assert(!mybot_json_get_integer(name, &integer));
    assert(!mybot_json_get_integer(mybot_json_get_object_item(object, "count"), NULL));

    mybot_json_t *replacement = mybot_json_create_string("robot");
    mybot_json_replace_item_in_object(object, "NAME", replacement);
    assert(strcmp(mybot_json_get_string(mybot_json_get_object_item(object, "name")), "robot") == 0);
    assert(strcmp(replacement->string, "NAME") == 0);
    replacement = mybot_json_create_null();
    mybot_json_replace_item_in_object(object, "missing", replacement);
    assert(replacement->string == NULL);
    mybot_json_delete(replacement);

    mybot_json_t *detached = mybot_json_detach_item_from_object(object, "COUNT");
    assert(detached != NULL && detached->string != NULL);
    mybot_json_delete(detached);
    assert(mybot_json_detach_item_from_object(object, "missing") == NULL);
    mybot_json_delete_item_from_object(object, "nothing");
    mybot_json_delete_item_from_object(object, "missing");
    assert(mybot_json_get_array_size(object) == 2);

    mybot_json_t *renamed = mybot_json_create_string("value");
    assert(mybot_json_add_item_to_object(object, "old", renamed) == 0);
    renamed = mybot_json_detach_item_from_object(object, "old");
    assert(mybot_json_add_item_to_object(object, "new", renamed) == 0);
    assert(strcmp(renamed->string, "new") == 0);

    assert(mybot_json_add_null(NULL, "x") == -1);
    assert(mybot_json_add_number(object, NULL, 1) == -1);
    assert(mybot_json_add_string(object, "x", NULL) == -1);
    assert(mybot_json_add_item(NULL, "x", renamed) == -1);
    assert(mybot_json_add_item(object, NULL, renamed) == -1);
    assert(mybot_json_add_item(object, "x", NULL) == -1);
    mybot_json_delete(object);
}

static void test_references_and_duplicate(void) {
    mybot_json_t *source = mybot_json_create_string("shared");
    mybot_json_t *array = mybot_json_create_array();
    mybot_json_t *object = mybot_json_create_object();
    assert(source != NULL && array != NULL && object != NULL);
    assert(mybot_json_add_item_reference_to_array(array, source) == 0);
    assert(mybot_json_add_item_reference_to_object(object, "alias", source) == 0);
    assert((array->child->type & MYBOT_JSON_IS_REFERENCE) != 0);
    assert(array->child->valuestring == source->valuestring);
    assert(object->child->valuestring == source->valuestring);
    mybot_json_delete(array);
    mybot_json_delete(object);
    assert(strcmp(source->valuestring, "shared") == 0);

    array = mybot_json_create_array();
    assert(mybot_json_add_item_reference_to_array(array, NULL) == -1);
    assert(mybot_json_add_item_reference_to_array(NULL, source) == -1);
    assert(mybot_json_add_item_reference_to_object(NULL, "alias", source) == -1);
    assert(mybot_json_add_item_reference_to_object(array, NULL, source) == -1);
    mybot_json_delete(array);
    mybot_json_delete(source);

    object = mybot_json_parse("{\"name\":\"original\",\"nested\":[1,2]}");
    assert(object != NULL);
    assert(mybot_json_duplicate(NULL, 1) == NULL);
    mybot_json_t *shallow = mybot_json_duplicate(object, 0);
    mybot_json_t *deep = mybot_json_duplicate(object, 1);
    assert(shallow != NULL && shallow->child == NULL);
    assert(deep != NULL && deep->child != object->child);
    assert(deep->child->string != object->child->string);
    assert(deep->child->valuestring != object->child->valuestring);
    memcpy(deep->child->valuestring, "changed", sizeof("changed"));
    assert(strcmp(object->child->valuestring, "original") == 0);
    assert(mybot_json_get_array_size(mybot_json_get_object_item(deep, "nested")) == 2);
    mybot_json_delete(shallow);
    mybot_json_delete(deep);
    mybot_json_delete(object);
}

static void test_formatted_print_and_minify(void) {
    mybot_json_t *object = mybot_json_parse("{\"items\":[true,false],\"empty\":{}}");
    assert(object != NULL);
    assert_printed(object, 1, "{\n\t\"items\":\t[true, false],\n\t\"empty\":\t{\n}\n}");
    assert_printed(object, 0, "{\"items\":[true,false],\"empty\":{}}");
    mybot_json_delete(object);

    object = mybot_json_parse("{\"outer\":{\"empty\":{}}}");
    assert(object != NULL);
    char *formatted = mybot_json_print(object);
    assert(formatted != NULL);
    assert(strstr(formatted, "\t\"empty\":\t{\n\t}\n\t}") != NULL);
    mybot_json_free_string(formatted);
    mybot_json_delete(object);

    char json[] = " /* head */ {\n \"text\" : \"a \\\" b\" , // line\n \"n\" : 1 /* tail */ } ";
    mybot_json_minify(json);
    assert(strcmp(json, "{\"text\":\"a \\\" b\",\"n\":1}") == 0);
}

typedef mybot_json_t *(*json_factory_fn)(void);

static mybot_json_t *create_int_array_fixture(void) {
    static const int values[] = {1, 2, 3};
    return mybot_json_create_int_array(values, 3);
}

static mybot_json_t *create_float_array_fixture(void) {
    static const float values[] = {1.0f, 2.0f, 3.0f};
    return mybot_json_create_float_array(values, 3);
}

static mybot_json_t *create_double_array_fixture(void) {
    static const double values[] = {1.0, 2.0, 3.0};
    return mybot_json_create_double_array(values, 3);
}

static mybot_json_t *create_string_array_fixture(void) {
    static const char *values[] = {"one", "two", "three"};
    return mybot_json_create_string_array(values, 3);
}

static void assert_factory_allocation_failures(json_factory_fn factory, size_t allocation_count) {
    for (size_t fail_at = 0; fail_at <= allocation_count; fail_at++) {
        reset_allocator_state();
        s_fail_on_attempt = fail_at;
        mybot_json_t *item = factory();
        assert((item != NULL) == (fail_at == allocation_count));
        mybot_json_delete(item);
        assert(s_successful_allocations == s_free_calls);
    }
}

static void test_composite_allocation_failures(void) {
    mybot_json_hooks_t hooks = {.malloc_fn = test_malloc, .free_fn = test_free};
    assert(mybot_json_init_hooks(&hooks) == 0);

    assert_factory_allocation_failures(create_int_array_fixture, 4);
    assert_factory_allocation_failures(create_float_array_fixture, 4);
    assert_factory_allocation_failures(create_double_array_fixture, 4);
    assert_factory_allocation_failures(create_string_array_fixture, 7);

    reset_allocator_state();
    mybot_json_t *source = mybot_json_create_string("source");
    mybot_json_t *array = mybot_json_create_array();
    assert(source != NULL && array != NULL);
    s_fail_on_attempt = s_allocation_attempts;
    assert(mybot_json_add_item_reference_to_array(array, source) == -1);
    assert(array->child == NULL);
    mybot_json_delete(array);
    mybot_json_delete(source);
    assert(s_successful_allocations == s_free_calls);

    reset_allocator_state();
    source = mybot_json_create_string("source");
    mybot_json_t *object = mybot_json_create_object();
    assert(source != NULL && object != NULL);
    s_fail_on_attempt = s_allocation_attempts + 1;
    assert(mybot_json_add_item_reference_to_object(object, "alias", source) == -1);
    assert(object->child == NULL);
    mybot_json_delete(object);
    mybot_json_delete(source);
    assert(s_successful_allocations == s_free_calls);

    reset_allocator_state();
    object = mybot_json_create_object();
    assert(object != NULL);
    s_fail_on_attempt = s_allocation_attempts + 1;
    assert(mybot_json_add_null(object, "value") == -1);
    assert(object->child == NULL);
    mybot_json_delete(object);
    assert(s_successful_allocations == s_free_calls);

    int duplicate_completed = 0;
    for (size_t fail_offset = 0; fail_offset < 32 && !duplicate_completed; fail_offset++) {
        reset_allocator_state();
        source = mybot_json_parse("{\"name\":\"source\",\"items\":[1,2]}");
        assert(source != NULL);
        s_fail_on_attempt = s_allocation_attempts + fail_offset;
        mybot_json_t *duplicate = mybot_json_duplicate(source, 1);
        if (duplicate) {
            duplicate_completed = 1;
            mybot_json_delete(duplicate);
        }
        mybot_json_delete(source);
        assert(s_successful_allocations == s_free_calls);
    }
    assert(duplicate_completed);

    int parse_completed = 0;
    for (size_t fail_at = 0; fail_at < 32 && !parse_completed; fail_at++) {
        reset_allocator_state();
        s_fail_on_attempt = fail_at;
        mybot_json_t *item = mybot_json_parse("{\"array\":[1,\"two\",null],\"flag\":true}");
        if (item) {
            parse_completed = 1;
            mybot_json_delete(item);
        }
        assert(s_successful_allocations == s_free_calls);
    }
    assert(parse_completed);

    int print_completed = 0;
    for (size_t fail_offset = 0; fail_offset < 32 && !print_completed; fail_offset++) {
        reset_allocator_state();
        mybot_json_t *item = mybot_json_parse("{\"array\":[1,\"two\"],\"flag\":true}");
        assert(item != NULL);
        s_fail_on_attempt = s_allocation_attempts + fail_offset;
        char *printed = mybot_json_print(item);
        if (printed) {
            print_completed = 1;
            mybot_json_free_string(printed);
        }
        mybot_json_delete(item);
        assert(s_successful_allocations == s_free_calls);
    }
    assert(print_completed);
    assert(mybot_json_init_hooks(NULL) == 0);
}

/* ---- deterministic parser fuzz ---- */
static uint32_t s_json_rng = 0x9e3779b9u;

static uint32_t json_next_rand(void) {
    s_json_rng ^= s_json_rng << 13;
    s_json_rng ^= s_json_rng >> 17;
    s_json_rng ^= s_json_rng << 5;
    return s_json_rng;
}

static void test_parse_fuzz(void) {
    static const char charset[] = "{}[],\"\\:0123456789abcdefghijklmnopqrstuvwxyz \t\n";
    mybot_json_hooks_t hooks = {.malloc_fn = test_malloc, .free_fn = test_free};
    reset_allocator_state();
    assert(mybot_json_init_hooks(&hooks) == 0);

    char buf[256];
    for (int iter = 0; iter < 20000; iter++) {
        int len = (int)(json_next_rand() % (sizeof(buf) - 1));
        for (int i = 0; i < len; i++) {
            buf[i] = charset[json_next_rand() % (sizeof(charset) - 1)];
        }
        buf[len] = '\0';

        mybot_json_t *root = mybot_json_parse(buf);
        if (root) {
            char *out = mybot_json_print_unformatted(root);
            if (out) {
                /* Printed output must re-parse cleanly. */
                mybot_json_t *again = mybot_json_parse(out);
                assert(again != NULL);
                mybot_json_delete(again);
                mybot_json_free_string(out);
            }
            mybot_json_delete(root);
        }
    }
    assert(s_successful_allocations == s_free_calls);
    assert(mybot_json_init_hooks(NULL) == 0);
}

int main(void) {
    test_round_trip();
    test_allocator_hooks();
    test_add_item_key_allocation_failure();
    test_string_value_allocation_failure();
    test_scalar_types_and_strings();
    test_parse_options_and_errors();
    test_array_builders();
    test_array_mutation();
    test_object_mutation_and_accessors();
    test_references_and_duplicate();
    test_formatted_print_and_minify();
    test_composite_allocation_failures();
    test_parse_fuzz();

    puts("json_test: ok");
    return 0;
}
