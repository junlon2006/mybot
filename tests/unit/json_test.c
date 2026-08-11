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
    test_parse_fuzz();

    puts("json_test: ok");
    return 0;
}
