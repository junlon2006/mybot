#include "storage/mybot_kv_store.h"

#include <api/aosl_log.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MYBOT_KV_STORE_DEFAULT_DIR ".mybot-kv-store"

typedef struct {
    char root[PATH_MAX];
} mybot_kv_store_file_ctx_t;

static bool valid_key(const char *key) {
    if (!key || !key[0]) {
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

static int make_path(const mybot_kv_store_file_ctx_t *ctx, const char *key, const char *suffix,
                     char *path, size_t path_size) {
    if (!valid_key(key)) {
        return -1;
    }
    int n = snprintf(path, path_size, "%s/%s%s", ctx->root, key, suffix ? suffix : "");
    return n >= 0 && (size_t)n < path_size ? 0 : -1;
}

static int kv_store_file_init(void **out_ctx) {
    if (!out_ctx) {
        return -1;
    }

    mybot_kv_store_file_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return -1;
    }

    const char *configured = getenv("MYBOT_KV_STORE_DIR");
    const char *root = configured && configured[0] ? configured : MYBOT_KV_STORE_DEFAULT_DIR;
    if (snprintf(ctx->root, sizeof(ctx->root), "%s", root) >= (int)sizeof(ctx->root)) {
        free(ctx);
        return -1;
    }

    if (mkdir(ctx->root, 0700) < 0 && errno != EEXIST) {
        AOSL_LOG_ERR("kv store: cannot create %s: %s", ctx->root, strerror(errno));
        free(ctx);
        return -1;
    }

    struct stat st;
    if (stat(ctx->root, &st) < 0 || !S_ISDIR(st.st_mode)) {
        AOSL_LOG_ERR("kv store: %s is not a directory", ctx->root);
        free(ctx);
        return -1;
    }
    (void)chmod(ctx->root, 0700);

    *out_ctx = ctx;
    AOSL_LOG_INF("kv store backend: %s", ctx->root);
    return 0;
}

static int kv_store_file_get(void *opaque, const char *key, void *value, size_t capacity,
                             size_t *out_len) {
    mybot_kv_store_file_ctx_t *ctx = opaque;
    char path[PATH_MAX];
    if (make_path(ctx, key, NULL, path, sizeof(path)) < 0) {
        return -1;
    }

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return errno == ENOENT ? MYBOT_KV_STORE_NOT_FOUND : -1;
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
    close(fd);
    if (extra_len != 0) {
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

static int kv_store_file_set(void *opaque, const char *key, const void *value, size_t len) {
    mybot_kv_store_file_ctx_t *ctx = opaque;
    char path[PATH_MAX];
    char temp_path[PATH_MAX];
    if (make_path(ctx, key, NULL, path, sizeof(path)) < 0 ||
        make_path(ctx, key, ".tmp", temp_path, sizeof(temp_path)) < 0) {
        return -1;
    }

    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
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
    if (ret == 0 && rename(temp_path, path) < 0) {
        ret = -1;
    }
    if (ret < 0) {
        (void)unlink(temp_path);
    }
    return ret;
}

static int kv_store_file_erase(void *opaque, const char *key) {
    mybot_kv_store_file_ctx_t *ctx = opaque;
    char path[PATH_MAX];
    if (make_path(ctx, key, NULL, path, sizeof(path)) < 0) {
        return -1;
    }
    return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
}

static void kv_store_file_destroy(void *ctx) {
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

void mybot_kv_store_platform_register_file(void) {
    if (mybot_kv_store_register(&s_kv_store_file_ops) < 0) {
        AOSL_LOG_ERR("kv store platform registration failed");
    }
}
