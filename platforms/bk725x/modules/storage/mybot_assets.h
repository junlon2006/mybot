/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_PROJECT_ASSETS_H_
#define MYBOT_PROJECT_ASSETS_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mybot_asset_s {
    const uint8_t *data;
    size_t size;
} mybot_asset_t;

/* Resolve a firmware-embedded asset by its mybot/assets/... path. */
int mybot_asset_find(const char *path, mybot_asset_t *asset);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_PROJECT_ASSETS_H_ */
