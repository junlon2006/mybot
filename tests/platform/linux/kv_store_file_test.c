/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE

#include <mybot/platform/mybot_kv_store.h>

#include "mybot_kv_store_internal.h"

#include "linux_platform_adapters.h"
#include "platform_test.h"

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static mybot_kv_store_t s_store;
static int s_fsync_count;

int __real_fsync(int fd);

int __wrap_fsync(int fd) {
    s_fsync_count++;
    return __real_fsync(fd);
}

static void join_path(char *out, size_t capacity, const char *dir, const char *name) {
    int written = snprintf(out, capacity, "%s/%s", dir, name);
    assert(written >= 0 && (size_t)written < capacity);
}

static void write_file(const char *path, const char *value) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    assert(fd >= 0);
    size_t len = strlen(value);
    assert(write(fd, value, len) == (ssize_t)len);
    assert(close(fd) == 0);
}

static void expect_file(const char *path, const char *expected) {
    char value[64];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    ssize_t len = read(fd, value, sizeof(value) - 1);
    assert(len >= 0);
    value[len] = '\0';
    assert(close(fd) == 0);
    assert(strcmp(value, expected) == 0);
}

static void expect_value(const char *key, const char *expected) {
    char value[64];
    size_t len = 0;
    assert(mybot_kv_store_get(&s_store, key, value, sizeof(value), &len) == 0);
    assert(len == strlen(expected));
    assert(memcmp(value, expected, len) == 0);
}

int main(void) {
    char base_template[] = "/tmp/mybot-kv-test-XXXXXX";
    char *base = mkdtemp(base_template);
    assert(base != NULL);

    char store[PATH_MAX];
    char victim[PATH_MAX];
    char key_path[PATH_MAX];
    char legacy_temp[PATH_MAX];
    char real_store[PATH_MAX];
    char linked_store[PATH_MAX];
    join_path(store, sizeof(store), base, "store");
    join_path(victim, sizeof(victim), base, "victim");
    join_path(key_path, sizeof(key_path), store, "device_auth");
    join_path(legacy_temp, sizeof(legacy_temp), store, "device_auth.tmp");
    join_path(real_store, sizeof(real_store), base, "real-store");
    join_path(linked_store, sizeof(linked_store), base, "linked-store");

    assert(setenv("MYBOT_KV_STORE_DIR", store, 1) == 0);
    mybot_platform_descriptor_t descriptor = mybot_test_platform_descriptor();
    descriptor.kv_store = linux_kv_store_platform_file_ops();
    assert(mybot_platform_register(&descriptor) == 0);
    assert(mybot_kv_store_init(&s_store) == 0);

    s_fsync_count = 0;
    assert(mybot_kv_store_set(&s_store, "device_auth", "first", 5) == 0);
    assert(s_fsync_count >= 2);
    expect_value("device_auth", "first");

    mybot_kv_store_deinit(&s_store);
    assert(mybot_kv_store_init(&s_store) == 0);
    expect_value("device_auth", "first");

    assert(mybot_kv_store_erase(&s_store, "device_auth") == 0);
    write_file(victim, "do-not-touch");
    assert(symlink(victim, key_path) == 0);
    assert(mybot_kv_store_set(&s_store, "device_auth", "attacker", 8) < 0);
    char value[64];
    size_t value_len = 0;
    assert(mybot_kv_store_get(&s_store, "device_auth", value, sizeof(value), &value_len) < 0);
    assert(mybot_kv_store_erase(&s_store, "device_auth") < 0);
    expect_file(victim, "do-not-touch");
    assert(unlink(key_path) == 0);

    assert(symlink(victim, legacy_temp) == 0);
    s_fsync_count = 0;
    assert(mybot_kv_store_set(&s_store, "device_auth", "second", 6) == 0);
    assert(s_fsync_count >= 2);
    expect_value("device_auth", "second");
    expect_file(victim, "do-not-touch");

    s_fsync_count = 0;
    assert(mybot_kv_store_erase(&s_store, "device_auth") == 0);
    assert(s_fsync_count >= 1);
    mybot_kv_store_deinit(&s_store);
    assert(mybot_kv_store_init(&s_store) == 0);
    assert(mybot_kv_store_get(&s_store, "device_auth", value, sizeof(value), &value_len) ==
           MYBOT_ERR_NOT_FOUND);
    mybot_kv_store_deinit(&s_store);

    assert(mkdir(real_store, 0700) == 0);
    assert(symlink(real_store, linked_store) == 0);
    assert(setenv("MYBOT_KV_STORE_DIR", linked_store, 1) == 0);
    assert(mybot_kv_store_init(&s_store) < 0);

    assert(unlink(linked_store) == 0);
    assert(unlink(legacy_temp) == 0);
    assert(unlink(victim) == 0);
    assert(rmdir(real_store) == 0);
    assert(rmdir(store) == 0);
    assert(rmdir(base) == 0);
    return 0;
}
