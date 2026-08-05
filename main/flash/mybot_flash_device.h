#ifndef MYBOT_FLASH_DEVICE_H_
#define MYBOT_FLASH_DEVICE_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYBOT_FLASH_NOT_FOUND 1

typedef struct {
    const char *name;
    int (*init)(void **ctx);
    int (*read)(void *ctx, const char *key, void *data, size_t capacity, size_t *out_len);
    int (*write)(void *ctx, const char *key, const void *data, size_t len);
    int (*erase)(void *ctx, const char *key);
    void (*destroy)(void *ctx);
} mybot_flash_ops_t;

int mybot_flash_register(const mybot_flash_ops_t *ops);
int mybot_flash_init(void);
void mybot_flash_deinit(void);
int mybot_flash_read(const char *key, void *data, size_t capacity, size_t *out_len);
int mybot_flash_write(const char *key, const void *data, size_t len);
int mybot_flash_erase(const char *key);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_FLASH_DEVICE_H_ */
