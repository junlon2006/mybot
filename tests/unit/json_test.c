/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_json.h"

#include <api/aosl.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_alloc_count;
static int s_fail_at;

static void *test_malloc(size_t size) {
    s_alloc_count++;
    if (s_fail_at > 0 && s_alloc_count == s_fail_at) {
        return NULL;
    }
    return malloc(size);
}

static void test_free(void *ptr) {
    free(ptr);
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

static void test_allocation_failure(void) {
    mybot_json_hooks_t hooks = {.malloc_fn = test_malloc, .free_fn = test_free};
    assert(mybot_json_init_hooks(&hooks) == 0);

    s_alloc_count = 0;
    s_fail_at = 1;
    assert(mybot_json_create_object() == NULL);

    s_fail_at = 0;
    mybot_json_t *root = mybot_json_create_object();
    assert(root != NULL);
    mybot_json_delete(root);
    assert(mybot_json_init_hooks(NULL) == 0);
}

int main(void) {
    aosl_ctor();
    test_parse_and_access();
    test_build_and_print();
    test_allocation_failure();
    aosl_dtor();
    puts("json_test: ok");
    return 0;
}
