#include "flash_device.h"

static const mybot_flash_ops_t *s_ops;
static void *s_ctx;

int mybot_flash_register(const mybot_flash_ops_t *ops)
{
    if (!ops || !ops->init || !ops->read || !ops->write || !ops->erase ||
        !ops->destroy) {
        return -1;
    }
    if (s_ctx) {
        return -1;
    }
    s_ops = ops;
    return 0;
}

int mybot_flash_init(void)
{
    if (s_ctx) {
        return 0;
    }
    if (!s_ops) {
        return -1;
    }
    return s_ops->init(&s_ctx);
}

void mybot_flash_deinit(void)
{
    if (s_ctx) {
        s_ops->destroy(s_ctx);
        s_ctx = NULL;
    }
}

int mybot_flash_read(const char *key, void *data, size_t capacity,
                     size_t *out_len)
{
    if (!s_ctx || !key || !data || !out_len) {
        return -1;
    }
    return s_ops->read(s_ctx, key, data, capacity, out_len);
}

int mybot_flash_write(const char *key, const void *data, size_t len)
{
    if (!s_ctx || !key || (!data && len != 0)) {
        return -1;
    }
    return s_ops->write(s_ctx, key, data, len);
}

int mybot_flash_erase(const char *key)
{
    if (!s_ctx || !key) {
        return -1;
    }
    return s_ops->erase(s_ctx, key);
}
