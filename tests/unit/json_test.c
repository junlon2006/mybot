/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_json.h"

#include <api/aosl.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_alloc_count;
static int s_fail_at;
static int s_live_alloc_count;

static void *test_malloc(size_t size) {
    s_alloc_count++;
    if (s_fail_at > 0 && s_alloc_count == s_fail_at) {
        return NULL;
    }
    void *ptr = malloc(size);
    if (ptr)
        s_live_alloc_count++;
    return ptr;
}

static void test_free(void *ptr) {
    if (ptr) {
        assert(s_live_alloc_count > 0);
        s_live_alloc_count--;
    }
    free(ptr);
}

static void use_test_allocator(void) {
    mybot_json_hooks_t hooks = {.malloc_fn = test_malloc, .free_fn = test_free};
    assert(s_live_alloc_count == 0);
    s_alloc_count = 0;
    s_fail_at = 0;
    assert(mybot_json_init_hooks(&hooks) == 0);
}

static void restore_allocator(void) {
    assert(s_live_alloc_count == 0);
    s_fail_at = 0;
    assert(mybot_json_init_hooks(NULL) == 0);
}

static void test_parse_and_access(void) {
    mybot_json_t *root = mybot_json_parse(
        "{\"name\":\"mybot\",\"count\":42,\"enabled\":true,\"nested\":{\"ok\":false}}");
    assert(root != NULL);
    assert(strcmp(mybot_json_get_string(mybot_json_get_object_item(root, "name")), "mybot") == 0);

    int64_t count = 0;
    assert(mybot_json_get_integer(mybot_json_get_object_item(root, "count"), &count));
    assert(count == 42);
    assert(mybot_json_get_object_item(root, "missing") == NULL);
    assert(mybot_json_get_string(mybot_json_get_object_item(root, "count")) == NULL);
    assert(!mybot_json_get_integer(NULL, &count));
    mybot_json_delete(root);

    assert(mybot_json_parse("{invalid") == NULL);
}

static void test_build_and_print(void) {
    mybot_json_t *root = mybot_json_create_object();
    mybot_json_t *details = mybot_json_create_object();
    assert(root && details);
    assert(mybot_json_add_string(root, "name", "mybot") == 0);
    assert(mybot_json_add_number(root, "count", 42) == 0);
    assert(mybot_json_add_bool(root, "enabled", true) == 0);
    assert(mybot_json_add_string(details, "implementation", "namespaced-json") == 0);
    assert(mybot_json_add_item(root, "details", details) == 0);

    char *printed = mybot_json_print_unformatted(root);
    assert(printed != NULL);
    assert(strstr(printed, "\"name\":\"mybot\"") != NULL);
    mybot_json_free_string(printed);
    mybot_json_delete(root);
}

static void test_arrays_escapes_and_numbers(void) {
    assert(mybot_json_parse(NULL) == NULL);
    mybot_json_t *root = mybot_json_parse("[null,false,true,-12.5e2,\"line\\nquote\\\"\\u4f60\"]");
    assert(root != NULL);
    assert(root->type == MYBOT_JSON_ARRAY);
    mybot_json_t *item = root->child;
    assert(item && item->type == MYBOT_JSON_NULL);
    item = item->next;
    assert(item && item->type == MYBOT_JSON_FALSE);
    item = item->next;
    assert(item && item->type == MYBOT_JSON_TRUE && item->valueint == 1);
    item = item->next;
    assert(item && item->type == MYBOT_JSON_NUMBER);
    assert(item->valuedouble == -1250.0);
    item = item->next;
    assert(item && item->type == MYBOT_JSON_STRING);
    assert(strcmp(item->valuestring, "line\nquote\"你") == 0);
    assert(item->next == NULL);

    char *printed = mybot_json_print_unformatted(root);
    assert(printed != NULL);
    mybot_json_t *round_trip = mybot_json_parse(printed);
    assert(round_trip != NULL);
    assert(round_trip->type == MYBOT_JSON_ARRAY);
    mybot_json_free_string(printed);
    mybot_json_delete(round_trip);
    mybot_json_delete(root);

    assert(mybot_json_parse("[1,]") == NULL);
    assert(mybot_json_parse("{\"value\":}") == NULL);
    assert(mybot_json_parse("-") == NULL);
    assert(mybot_json_parse("1e") == NULL);
    assert(mybot_json_parse("1e20") == NULL);
    assert(mybot_json_parse("1.") == NULL);
    assert(mybot_json_parse("01") == NULL);
    assert(mybot_json_parse("1 trailing") == NULL);
    assert(mybot_json_parse("\v1") == NULL);

    /* Strict RFC 8259 string validation. */
    assert(mybot_json_parse("\"bad\\q\"") == NULL);
    assert(mybot_json_parse("\"bad\nline\"") == NULL);
    assert(mybot_json_parse("\"\\uD800\"") == NULL);
    assert(mybot_json_parse("\"\\uDC00\"") == NULL);
    assert(mybot_json_parse("\"\\uD800\\u0041\"") == NULL);

    char nested[80];
    size_t pos = 0;
    for (int i = 0; i < 33; ++i)
        nested[pos++] = '[';
    nested[pos++] = '0';
    for (int i = 0; i < 33; ++i)
        nested[pos++] = ']';
    nested[pos] = '\0';
    assert(mybot_json_parse(nested) == NULL);
}

static void test_empty_containers_and_number_formats(void) {
    mybot_json_t *array = mybot_json_parse("[]");
    assert(array != NULL && array->type == MYBOT_JSON_ARRAY && array->child == NULL);
    char *printed = mybot_json_print_unformatted(array);
    assert(printed != NULL && strcmp(printed, "[]") == 0);
    mybot_json_free_string(printed);
    mybot_json_delete(array);

    mybot_json_t *object = mybot_json_parse("{}");
    assert(object != NULL && object->type == MYBOT_JSON_OBJECT && object->child == NULL);
    printed = mybot_json_print_unformatted(object);
    assert(printed != NULL && strcmp(printed, "{}") == 0);
    mybot_json_free_string(printed);
    mybot_json_delete(object);

    object = mybot_json_create_object();
    assert(object != NULL);
    assert(mybot_json_add_number(object, "fraction", 1.5) == 0);
    assert(mybot_json_add_number(object, "small", 0.0000001) == 0);
    assert(mybot_json_add_number(object, "large", 10000000000.0) == 0);
    printed = mybot_json_print_unformatted(object);
    assert(printed != NULL);
    assert(strstr(printed, "fraction") != NULL);
    mybot_json_free_string(printed);
    mybot_json_delete(object);

    mybot_json_t *escaped = mybot_json_parse("\"\\b\\f\\r\\t\\u0061\\uD83D\\uDE00\"");
    assert(escaped != NULL && escaped->type == MYBOT_JSON_STRING);
    const char expected[] = "\b\f\r\ta\xF0\x9F\x98\x80";
    assert(strcmp(escaped->valuestring, expected) == 0);
    mybot_json_delete(escaped);
}

static void test_number_boundaries(void) {
    static const struct {
        const char *text;
        int64_t integer;
        double number;
    } valid[] = {
        {"9223372036854775807", INT64_MAX, (double)INT64_MAX},
        {"-9223372036854775808", INT64_MIN, (double)INT64_MIN},
        {"-0", 0, 0.0},
        {"-0.125", 0, -0.125},
        {"12.5E+1", 125, 125.0},
        {"125e-2", 1, 1.25},
        {"-9223372036854775808e0", INT64_MIN, (double)INT64_MIN},
        {"0e-2147483647", 0, 0.0},
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        mybot_json_t *value = mybot_json_parse(valid[i].text);
        assert(value != NULL && value->type == MYBOT_JSON_NUMBER);
        int64_t integer = 7;
        assert(mybot_json_get_integer(value, &integer));
        assert(integer == valid[i].integer);
        assert(value->valuedouble == valid[i].number);
        assert(!mybot_json_get_integer(value, NULL));
        mybot_json_delete(value);
    }

    static const char *invalid[] = {
        "9223372036854775808",
        "-9223372036854775809",
        "18446744073709551616",
        "9.223372036854776e18",
        "-9.223372036854778e18",
        "1e309",
        "1e2147483648",
        "1e+",
        "1e-",
        "+1",
        ".1",
        "--1",
        "00",
        "-01",
        "NaN",
        "Infinity",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(mybot_json_parse(invalid[i]) == NULL);
}

static void test_string_validation_and_round_trip(void) {
    mybot_json_t *unicode = mybot_json_parse("\"\\u007f\\u00aF\\u07ff\\u0800\\uFFFF\\/\\\\\"");
    const char expected[] = "\x7f\xc2\xaf\xdf\xbf\xe0\xa0\x80\xef\xbf\xbf/\\";
    assert(unicode != NULL);
    assert(strcmp(mybot_json_get_string(unicode), expected) == 0);
    mybot_json_delete(unicode);

    unicode = mybot_json_parse("\"\\uabcd\\uABCD\\ud83d\\ude00\"");
    assert(unicode != NULL);
    assert(strcmp(mybot_json_get_string(unicode), "\xea\xaf\x8d\xea\xaf\x8d\xf0\x9f\x98\x80") == 0);
    mybot_json_delete(unicode);

    const char controls[] = "\"\\\b\f\n\r\t\x01\x1f/";
    mybot_json_t *string = mybot_json_create_string(controls);
    assert(string != NULL);
    char *printed = mybot_json_print_unformatted(string);
    assert(printed != NULL);
    assert(strcmp(printed, "\"\\\"\\\\\\b\\f\\n\\r\\t\\u0001\\u001f/\"") == 0);
    mybot_json_t *round_trip = mybot_json_parse(printed);
    assert(round_trip != NULL);
    assert(strcmp(mybot_json_get_string(round_trip), controls) == 0);
    mybot_json_delete(round_trip);
    mybot_json_free_string(printed);
    mybot_json_delete(string);

    static const char *invalid[] = {
        "\"unterminated",     "\"trailing\\",       "\"\\u\"",
        "\"\\u12\"",          "\"\\u0000\"",        "\"\\ux123\"",
        "\"\\u1x23\"",        "\"\\u12x3\"",        "\"\\u123x\"",
        "\"\\uD800x\"",       "\"\\uD800\\xDC00\"", "\"\\uD800\\uDC\"",
        "\"\\uD800\\uXC00\"", "\"\\uD800\\uDX00\"", "\"\\uD800\\uDCX0\"",
        "\"\\uD800\\uDC0X\"", "\"\\uD800\\uE000\"",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(mybot_json_parse(invalid[i]) == NULL);
}

static void test_container_validation_and_depth(void) {
    mybot_json_t *root = mybot_json_parse(" \t\r\n { \"FiRsT\" : 1, \"last\" : [ true ] } \t\r\n");
    assert(root != NULL);
    assert(mybot_json_get_object_item(root, "first") == root->child);
    assert(mybot_json_get_object_item(root, "LAST") == root->child->next);
    assert(root->child->prev == NULL && root->child->next->prev == root->child);
    assert(mybot_json_get_object_item(NULL, "first") == NULL);
    assert(mybot_json_get_object_item(root, NULL) == NULL);
    int64_t integer = 7;
    assert(!mybot_json_get_integer(root, &integer) && integer == 7);
    assert(mybot_json_get_string(NULL) == NULL);
    mybot_json_delete(root);

    static const char *invalid[] = {
        "",
        " \t\r\n",
        "[",
        "[1",
        "[1 2]",
        "[1,,2]",
        "[1,?]",
        "{",
        "{key:1}",
        "{\"a\" 1}",
        "{\"a\":1",
        "{\"a\":1,}",
        "{\"a\":1,2}",
        "{\"a\":1,\"b\" 2}",
        "{\"a\":1,\"b\":?}",
        "{\"a\":1 \"b\":2}",
        "nullx",
        "truefalse",
        "{}[]",
        "\fnull",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(mybot_json_parse(invalid[i]) == NULL);

    for (int object = 0; object <= 1; ++object) {
        for (int depth = 32; depth <= 33; ++depth) {
            char nested[256];
            size_t pos = 0;
            for (int i = 0; i < depth; ++i) {
                if (object) {
                    memcpy(nested + pos, "{\"a\":", 5);
                    pos += 5;
                } else {
                    nested[pos++] = '[';
                }
            }
            nested[pos++] = '0';
            for (int i = 0; i < depth; ++i)
                nested[pos++] = object ? '}' : ']';
            nested[pos] = '\0';
            root = mybot_json_parse(nested);
            assert((root != NULL) == (depth == 32));
            mybot_json_delete(root);
        }
    }
}

static void test_builders_copy_input(void) {
    char name[] = "name";
    char text[] = "value";
    mybot_json_t *object = mybot_json_create_object();
    mybot_json_t *string = mybot_json_create_string(text);
    assert(object != NULL && string != NULL);
    assert(mybot_json_add_item(object, name, string) == 0);
    memset(name, 'x', strlen(name));
    memset(text, 'x', strlen(text));
    assert(mybot_json_get_object_item(object, "name") == string);
    assert(strcmp(mybot_json_get_string(string), "value") == 0);
    assert(mybot_json_add_bool(object, "disabled", false) == 0);
    assert(mybot_json_add_number(object, "negative", -42) == 0);
    assert(mybot_json_add_string(object, "empty", "") == 0);
    char *printed = mybot_json_print_unformatted(object);
    assert(printed != NULL);
    assert(strcmp(printed,
                  "{\"name\":\"value\",\"disabled\":false,\"negative\":-42,\"empty\":\"\"}") == 0);
    mybot_json_free_string(printed);
    mybot_json_delete(object);

    mybot_json_t *value = mybot_json_create_bool(5);
    assert(value != NULL && value->type == MYBOT_JSON_TRUE);
    mybot_json_delete(value);
    assert(mybot_json_create_string(NULL) == NULL);
    assert(mybot_json_print_unformatted(NULL) == NULL);
    mybot_json_delete(NULL);
    mybot_json_free_string(NULL);
}

static void assert_parse_allocation_failures(const char *text) {
    s_alloc_count = 0;
    s_fail_at = 0;
    mybot_json_t *root = mybot_json_parse(text);
    assert(root != NULL);
    const int allocation_count = s_alloc_count;
    mybot_json_delete(root);
    assert(s_live_alloc_count == 0);

    for (int fail_at = 1; fail_at <= allocation_count; ++fail_at) {
        s_alloc_count = 0;
        s_fail_at = fail_at;
        assert(mybot_json_parse(text) == NULL);
        assert(s_live_alloc_count == 0);
    }
    s_fail_at = 0;
    root = mybot_json_parse(text);
    assert(root != NULL);
    mybot_json_delete(root);
    assert(s_live_alloc_count == 0);
}

static void assert_print_allocation_failures(const char *text) {
    s_fail_at = 0;
    mybot_json_t *root = mybot_json_parse(text);
    assert(root != NULL);
    const int root_allocations = s_live_alloc_count;
    s_alloc_count = 0;
    char *printed = mybot_json_print_unformatted(root);
    assert(printed != NULL);
    const int allocation_count = s_alloc_count;
    mybot_json_free_string(printed);
    assert(s_live_alloc_count == root_allocations);

    for (int fail_at = 1; fail_at <= allocation_count; ++fail_at) {
        s_alloc_count = 0;
        s_fail_at = fail_at;
        assert(mybot_json_print_unformatted(root) == NULL);
        assert(s_live_alloc_count == root_allocations);
    }
    s_fail_at = 0;
    printed = mybot_json_print_unformatted(root);
    assert(printed != NULL && strcmp(printed, text) == 0);
    mybot_json_free_string(printed);
    mybot_json_delete(root);
    assert(s_live_alloc_count == 0);
}

static void test_numeric_regressions(void) {
    use_test_allocator();
    const double invalid[] = {NAN, INFINITY, -INFINITY, 9223372036854775808.0,
                              nextafter(-9223372036854775808.0, -INFINITY)};
    mybot_json_t *object = mybot_json_create_object();
    assert(object != NULL);
    const int object_allocations = s_alloc_count;
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(mybot_json_create_number(invalid[i]) == NULL);
        assert(mybot_json_add_number(object, "invalid", invalid[i]) < 0);
        assert(object->child == NULL && s_live_alloc_count == 1);
        assert(s_alloc_count == object_allocations);
    }
    mybot_json_delete(object);

    const struct {
        double number;
        int64_t integer;
    } valid[] = {{-9223372036854775808.0, INT64_MIN},
                 {nextafter(9223372036854775808.0, 0.0), INT64_MAX - 1023},
                 {-1.5, -1}};
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        mybot_json_t *value = mybot_json_create_number(valid[i].number);
        assert(value != NULL && value->valuedouble == valid[i].number);
        int64_t integer = 0;
        assert(mybot_json_get_integer(value, &integer) && integer == valid[i].integer);
        mybot_json_delete(value);
        assert(s_live_alloc_count == 0);
    }

    static const char *underflow[] = {"0.01e-2147483647", "-0.01e-2147483647"};
    for (size_t i = 0; i < sizeof(underflow) / sizeof(underflow[0]); ++i) {
        mybot_json_t *value = mybot_json_parse(underflow[i]);
        assert(value != NULL && value->type == MYBOT_JSON_NUMBER);
        assert(value->valuedouble == 0.0 && value->valueint == 0);
        assert((signbit(value->valuedouble) != 0) == (i == 1));
        mybot_json_delete(value);
        assert(s_live_alloc_count == 0);
    }

    static const struct {
        const char *text;
        int64_t integer;
    } integers[] = {{"9223372036854775807", INT64_MAX},
                    {"-9223372036854775808", INT64_MIN},
                    {"9007199254740993", INT64_C(9007199254740993)},
                    {"-9007199254740993", -INT64_C(9007199254740993)}};
    for (size_t i = 0; i < sizeof(integers) / sizeof(integers[0]); ++i) {
        mybot_json_t *value = mybot_json_parse(integers[i].text);
        assert(value != NULL);
        char *printed = mybot_json_print_unformatted(value);
        assert(printed != NULL && strcmp(printed, integers[i].text) == 0);
        mybot_json_t *round_trip = mybot_json_parse(printed);
        assert(round_trip != NULL);
        int64_t integer = 0;
        assert(mybot_json_get_integer(round_trip, &integer) && integer == integers[i].integer);
        mybot_json_delete(round_trip);
        mybot_json_free_string(printed);
        mybot_json_delete(value);
        assert(s_live_alloc_count == 0);
    }
    assert_print_allocation_failures("9223372036854775807");
    assert_print_allocation_failures("-9223372036854775808");
    restore_allocator();
}

static void test_parse_and_print_allocation_failures(void) {
    use_test_allocator();
    assert_parse_allocation_failures("{\"a\":[\"one\",{\"two\":2}],\"b\":false,\"c\":\"three\"}");
    assert_parse_allocation_failures("[1,\"two\",{\"three\":[4]}]");

    static const char *printable[] = {
        "{}",
        "[]",
        "null",
        "false",
        "true",
        "1",
        "1.5",
        "\"text\"",
        "[1,\"two\",{\"three\":3}]",
        "{\"a\":[1,2],\"b\":false,\"c\":\"three\"}",
    };
    for (size_t i = 0; i < sizeof(printable) / sizeof(printable[0]); ++i)
        assert_print_allocation_failures(printable[i]);
    restore_allocator();
}

static void test_builder_failure_ownership(void) {
    use_test_allocator();
    mybot_json_t *object = mybot_json_create_object();
    mybot_json_t *item = mybot_json_create_string("owned by caller");
    assert(object != NULL && item != NULL);
    const int initial_allocations = s_live_alloc_count;
    assert(mybot_json_add_item(NULL, "name", item) < 0);
    assert(mybot_json_add_item(object, NULL, item) < 0);
    assert(mybot_json_add_item(object, "name", NULL) < 0);
    assert(object->child == NULL && item->string == NULL);
    assert(s_live_alloc_count == initial_allocations);

    s_alloc_count = 0;
    s_fail_at = 1;
    assert(mybot_json_add_item(object, "name", item) < 0);
    assert(object->child == NULL && item->string == NULL);
    assert(strcmp(mybot_json_get_string(item), "owned by caller") == 0);
    assert(s_live_alloc_count == initial_allocations);
    s_fail_at = 0;
    assert(mybot_json_add_item(object, "name", item) == 0);
    assert(object->child == item);
    mybot_json_delete(object);
    assert(s_live_alloc_count == 0);

    /* A detached parsed child already owns a key; adopting it replaces that key. */
    mybot_json_t *previous = mybot_json_parse("{\"old\":\"value\"}");
    assert(previous != NULL);
    item = previous->child;
    previous->child = NULL;
    mybot_json_delete(previous);
    object = mybot_json_create_object();
    assert(object != NULL);
    const int renamed_allocations = s_live_alloc_count;
    char *old_name = item->string;
    s_alloc_count = 0;
    s_fail_at = 1;
    assert(mybot_json_add_item(object, "new", item) < 0);
    assert(object->child == NULL && item->string == old_name);
    assert(strcmp(item->string, "old") == 0);
    assert(s_live_alloc_count == renamed_allocations);
    s_fail_at = 0;
    assert(mybot_json_add_item(object, "new", item) == 0);
    assert(mybot_json_get_object_item(object, "new") == item);
    assert(mybot_json_get_object_item(object, "old") == NULL);
    assert(s_live_alloc_count == renamed_allocations);
    mybot_json_delete(object);
    assert(s_live_alloc_count == 0);

    object = mybot_json_create_object();
    assert(object != NULL);
    assert(mybot_json_add_string(object, "name", NULL) < 0);
    assert(mybot_json_add_string(NULL, "name", "value") < 0);
    assert(mybot_json_add_string(object, NULL, "value") < 0);
    assert(mybot_json_add_number(NULL, "name", 1) < 0);
    assert(mybot_json_add_bool(object, NULL, false) < 0);
    assert(s_live_alloc_count == 1 && object->child == NULL);

    for (int fail_at = 1; fail_at <= 3; ++fail_at) {
        s_alloc_count = 0;
        s_fail_at = fail_at;
        assert(mybot_json_add_string(object, "name", "value") < 0);
        assert(s_live_alloc_count == 1 && object->child == NULL);
    }
    for (int fail_at = 1; fail_at <= 2; ++fail_at) {
        s_alloc_count = 0;
        s_fail_at = fail_at;
        assert(mybot_json_add_number(object, "number", 1) < 0);
        assert(s_live_alloc_count == 1 && object->child == NULL);
        s_alloc_count = 0;
        assert(mybot_json_add_bool(object, "bool", true) < 0);
        assert(s_live_alloc_count == 1 && object->child == NULL);
    }
    s_fail_at = 0;
    assert(mybot_json_add_string(object, "name", "value") == 0);
    mybot_json_delete(object);
    restore_allocator();
}

static void test_allocation_failure(void) {
    mybot_json_hooks_t invalid_hooks = {.malloc_fn = NULL, .free_fn = test_free};
    assert(mybot_json_init_hooks(&invalid_hooks) < 0);
    use_test_allocator();
    invalid_hooks.malloc_fn = test_malloc;
    invalid_hooks.free_fn = NULL;
    assert(mybot_json_init_hooks(&invalid_hooks) < 0);

    s_alloc_count = 0;
    s_fail_at = 1;
    assert(mybot_json_create_object() == NULL);

    s_fail_at = 0;
    mybot_json_t *root = mybot_json_create_object();
    assert(root != NULL);
    mybot_json_delete(root);
    restore_allocator();
}

int main(void) {
    aosl_ctor();
    test_parse_and_access();
    test_build_and_print();
    test_arrays_escapes_and_numbers();
    test_empty_containers_and_number_formats();
    test_number_boundaries();
    test_string_validation_and_round_trip();
    test_container_validation_and_depth();
    test_builders_copy_input();
    test_numeric_regressions();
    test_parse_and_print_allocation_failures();
    test_builder_failure_ownership();
    test_allocation_failure();
    aosl_dtor();
    puts("json_test: ok");
    return 0;
}
