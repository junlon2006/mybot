/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_VIDEO_INTERNAL_H_
#define MYBOT_VIDEO_INTERNAL_H_

#include <api/aosl_atomic.h>

#include <mybot/platform/mybot_video.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const mybot_video_ops_t *ops;
    void *ctx;
    uint32_t min_bps;
    uint32_t max_bps;
    bool initialized;
    aosl_atomic_t active;
    aosl_atomic_t stopping;
} mybot_video_t;

void mybot_video_init(mybot_video_t *video);
int mybot_video_start(mybot_video_t *video);
int mybot_video_stop(mybot_video_t *video);
int mybot_video_destroy(mybot_video_t *video);
void mybot_video_request_key_frame(mybot_video_t *video);
void mybot_video_set_target_bitrate(mybot_video_t *video, uint32_t target_bps);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_VIDEO_INTERNAL_H_ */
