/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE

#include <mybot/platform/mybot_kv_store.h>

#include "linux_backends.h"

#include <api/aosl_log.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#define MYBOT_KV_STORE_DEFAULT_DIR ".mybot-kv-store"
#define MYBOT_KV_TEMP_ATTEMPTS 16

typedef struct {
    int dir_fd;
    char root[PATH_MAX];
} linux_kv_store_file_ctx_t;

static bool valid_key(const char *key) {
    if (!key || !key[0] || strcmp(key, ".") == 0 || strcmp(key, "..") == 0 ||
        strlen(key) > NAME_MAX - 32) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '_' || *p == '-' || *p == '.')) {
            return false;
        }
    }
    return true;
}

static int open_store_directory(const char *root) {
    char path[PATH_MAX];
    char *parts[PATH_MAX / 2];
    size_t part_count = 0;
    char *save = NULL;

    if (!root || !root[0] || snprintf(path, sizeof(path), "%s", root) >= (int)sizeof(path)) {
        return -1;
    }

    for (char *part = strtok_r(path, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (strcmp(part, ".") == 0) {
            continue;
        }
        if (strcmp(part, "..") == 0 || part_count >= sizeof(parts) / sizeof(parts[0])) {
            return -1;
        }
        parts[part_count++] = part;
    }
    if (part_count == 0) {
        return -1;
    }

    int dir_fd = open(root[0] == '/' ? "/" : ".", O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (dir_fd < 0) {
        return -1;
    }

    for (size_t i = 0; i < part_count; i++) {
        bool created = false;
        if (i + 1 == part_count && mkdirat(dir_fd, parts[i], 0700) == 0) {
            created = true;
        } else if (i + 1 == part_count && errno != EEXIST) {
            close(dir_fd);
            return -1;
        }

        int next_fd = openat(dir_fd, parts[i], O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        if (next_fd < 0) {
            close(dir_fd);
            return -1;
        }
        if (created && fsync(dir_fd) < 0) {
            close(next_fd);
            close(dir_fd);
            return -1;
        }
        close(dir_fd);
        dir_fd = next_fd;
    }

    struct stat st;
    if (fstat(dir_fd, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
        fchmod(dir_fd, 0700) < 0 || fsync(dir_fd) < 0) {
        close(dir_fd);
        return -1;
    }
    return dir_fd;
}

static int kv_store_file_init(void **out_ctx) {
    if (!out_ctx) {
        return -1;
    }

    linux_kv_store_file_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return -1;
    }
    ctx->dir_fd = -1;

    const char *configured = getenv("MYBOT_KV_STORE_DIR");
    const char *root = configured && configured[0] ? configured : MYBOT_KV_STORE_DEFAULT_DIR;
    if (snprintf(ctx->root, sizeof(ctx->root), "%s", root) >= (int)sizeof(ctx->root)) {
        free(ctx);
        return -1;
    }

    ctx->dir_fd = open_store_directory(ctx->root);
    if (ctx->dir_fd < 0) {
        AOSL_LOG_ERR("kv store: cannot securely open %s: %s", ctx->root, strerror(errno));
        free(ctx);
        return -1;
    }

    *out_ctx = ctx;
    AOSL_LOG_INF("kv store backend: %s", ctx->root);
    return 0;
}

static int kv_store_file_get(void *opaque, const char *key, void *value, size_t capacity,
                             size_t *out_len) {
    linux_kv_store_file_ctx_t *ctx = opaque;
    if (!ctx || !valid_key(key)) {
        return -1;
    }

    int fd = openat(ctx->dir_fd, key, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return errno == ENOENT ? MYBOT_ERR_NOT_FOUND : -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid()) {
        close(fd);
        return -1;
    }

    size_t total = 0;
    while (total < capacity) {
        ssize_t n = read(fd, (char *)value + total, capacity - total);
        if (n > 0) {
            total += (size_t)n;
        } else if (n == 0) {
            break;
        } else if (errno != EINTR) {
            close(fd);
            return -1;
        }
    }

    char extra;
    ssize_t extra_len;
    do {
        extra_len = read(fd, &extra, 1);
    } while (extra_len < 0 && errno == EINTR);
    int close_ret = close(fd);
    if (extra_len != 0 || close_ret < 0) {
        return -1;
    }

    *out_len = total;
    return 0;
}

static int write_all(int fd, const void *data, size_t len) {
    const char *p = data;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

static int create_temp_file(linux_kv_store_file_ctx_t *ctx, const char *key, char *temp_name,
                            size_t temp_name_size) {
    for (int attempt = 0; attempt < MYBOT_KV_TEMP_ATTEMPTS; attempt++) {
        uint64_t nonce;
        ssize_t random_len;
        do {
            random_len = getrandom(&nonce, sizeof(nonce), 0);
        } while (random_len < 0 && errno == EINTR);
        if (random_len != (ssize_t)sizeof(nonce)) {
            return -1;
        }

        int written =
            snprintf(temp_name, temp_name_size, ".%s.tmp.%016llx", key, (unsigned long long)nonce);
        if (written < 0 || (size_t)written >= temp_name_size) {
            return -1;
        }

        int fd = openat(ctx->dir_fd, temp_name,
                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) {
            return fd;
        }
        if (errno != EEXIST) {
            return -1;
        }
    }
    return -1;
}

static int existing_value_is_safe(linux_kv_store_file_ctx_t *ctx, const char *key) {
    struct stat st;
    if (fstatat(ctx->dir_fd, key, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        return S_ISREG(st.st_mode) && st.st_uid == geteuid() ? 0 : -1;
    }
    return errno == ENOENT ? 0 : -1;
}

static int kv_store_file_set(void *opaque, const char *key, const void *value, size_t len) {
    linux_kv_store_file_ctx_t *ctx = opaque;
    char temp_name[NAME_MAX + 1];
    if (!ctx || !valid_key(key) || existing_value_is_safe(ctx, key) < 0) {
        return -1;
    }

    int fd = create_temp_file(ctx, key, temp_name, sizeof(temp_name));
    if (fd < 0) {
        return -1;
    }

    int ret = write_all(fd, value, len);
    if (ret == 0 && fsync(fd) < 0) {
        ret = -1;
    }
    if (close(fd) < 0) {
        ret = -1;
    }
    if (ret == 0 && renameat(ctx->dir_fd, temp_name, ctx->dir_fd, key) < 0) {
        ret = -1;
    }
    if (ret == 0 && fsync(ctx->dir_fd) < 0) {
        ret = -1;
    }
    if (ret < 0) {
        if (unlinkat(ctx->dir_fd, temp_name, 0) == 0) {
            (void)fsync(ctx->dir_fd);
        }
    }
    return ret;
}

static int kv_store_file_erase(void *opaque, const char *key) {
    linux_kv_store_file_ctx_t *ctx = opaque;
    struct stat st;
    if (!ctx || !valid_key(key)) {
        return -1;
    }
    if (fstatat(ctx->dir_fd, key, &st, AT_SYMLINK_NOFOLLOW) < 0) {
        return errno == ENOENT ? 0 : -1;
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid()) {
        return -1;
    }
    if (unlinkat(ctx->dir_fd, key, 0) < 0) {
        return -1;
    }
    return fsync(ctx->dir_fd) == 0 ? 0 : -1;
}

static void kv_store_file_destroy(void *opaque) {
    linux_kv_store_file_ctx_t *ctx = opaque;
    if (!ctx) {
        return;
    }
    if (ctx->dir_fd >= 0) {
        close(ctx->dir_fd);
    }
    free(ctx);
}

static const mybot_kv_store_ops_t s_kv_store_file_ops = {
    .name = "file",
    .init = kv_store_file_init,
    .get = kv_store_file_get,
    .set = kv_store_file_set,
    .erase = kv_store_file_erase,
    .destroy = kv_store_file_destroy,
};

int linux_kv_store_platform_register_file(void) {
    int ret = mybot_kv_store_register(&s_kv_store_file_ops);
    if (ret < 0) {
        AOSL_LOG_ERR("kv store platform registration failed");
    }
    return ret;
}
