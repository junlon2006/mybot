/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_video_internal.h"

#include "mybot_agora_rtc.h"
#include "mybot_platform_registry.h"

#include <mybot/mybot_build_config.h>

#include <api/aosl_log.h>

#include <string.h>

#if MYBOT_ENABLE_VIDEO
static int video_frame_handler(const mybot_video_frame_t *frame, void *user_data) {
    mybot_video_t *video = (mybot_video_t *)user_data;
    if (!video || !frame || !aosl_atomic_read(&video->active) ||
        aosl_atomic_read(&video->stopping)) {
        return -1;
    }
    return mybot_agora_rtc_send_video(frame);
}
#endif

void mybot_video_init(mybot_video_t *video) {
    if (!video) {
        return;
    }

    if (video->initialized) {
        AOSL_LOG_ERR("video source is still initialized");
        return;
    }

    memset(video, 0, sizeof(*video));
    aosl_atomic_set(&video->active, false);
    aosl_atomic_set(&video->stopping, false);
#if MYBOT_ENABLE_VIDEO
    const mybot_platform_descriptor_t *platform = mybot_platform_registry_get();
    video->ops = platform ? platform->video : NULL;
    if (!video->ops) {
        AOSL_LOG_ERR("video platform operations are unavailable");
        return;
    }
    if (video->ops->max_bps == 0 || video->ops->max_bps < video->ops->min_bps) {
        AOSL_LOG_ERR("video platform bitrate range is invalid (min=%u, max=%u)",
                     (unsigned int)video->ops->min_bps, (unsigned int)video->ops->max_bps);
        video->ops = NULL;
        return;
    }
    video->min_bps = video->ops->min_bps;
    video->max_bps = video->ops->max_bps;
    if (video->ops->init(&video->ctx, video_frame_handler, video) < 0) {
        video->ctx = NULL;
        video->ops = NULL;
        AOSL_LOG_ERR("video platform initialization failed");
        return;
    }
    video->initialized = true;
#else
    (void)video;
#endif
}

int mybot_video_start(mybot_video_t *video) {
#if MYBOT_ENABLE_VIDEO
    if (!video || !video->initialized || !video->ops || aosl_atomic_read(&video->stopping)) {
        return -1;
    }
    if (aosl_atomic_read(&video->active)) {
        return 0;
    }
    /* Mark active before start so a source that emits immediately is accepted. */
    aosl_atomic_set(&video->active, true);
    if (video->ops->start(video->ctx) < 0) {
        aosl_atomic_set(&video->active, false);
        AOSL_LOG_ERR("video platform start failed");
        return -1;
    }
    return 0;
#else
    (void)video;
    return 0;
#endif
}

int mybot_video_stop(mybot_video_t *video) {
#if MYBOT_ENABLE_VIDEO
    if (!video || !video->initialized || !video->ops) {
        return 0;
    }
    if (!aosl_atomic_read(&video->active) && !aosl_atomic_read(&video->stopping)) {
        return 0;
    }
    aosl_atomic_set(&video->active, false);
    aosl_atomic_set(&video->stopping, true);
    if (video->ops->stop(video->ctx) < 0) {
        AOSL_LOG_ERR("video platform stop incomplete");
        return -1;
    }
    aosl_atomic_set(&video->stopping, false);
    return 0;
#else
    (void)video;
    return 0;
#endif
}

int mybot_video_destroy(mybot_video_t *video) {
#if MYBOT_ENABLE_VIDEO
    if (!video) {
        return -1;
    }
    if (!video->initialized) {
        return 0;
    }
    if (aosl_atomic_read(&video->active) || aosl_atomic_read(&video->stopping)) {
        AOSL_LOG_ERR("cannot destroy active video source");
        return -1;
    }
    video->ops->destroy(video->ctx);
    video->ctx = NULL;
    video->ops = NULL;
    video->initialized = false;
    return 0;
#else
    (void)video;
    return 0;
#endif
}

void mybot_video_request_key_frame(mybot_video_t *video) {
#if MYBOT_ENABLE_VIDEO
    if (video && video->initialized && aosl_atomic_read(&video->active) &&
        !aosl_atomic_read(&video->stopping) && video->ops && video->ops->on_key_frame_request) {
        video->ops->on_key_frame_request(video->ctx);
    }
#else
    (void)video;
#endif
}

void mybot_video_set_target_bitrate(mybot_video_t *video, uint32_t target_bps) {
#if MYBOT_ENABLE_VIDEO
    if (video && video->initialized && aosl_atomic_read(&video->active) &&
        !aosl_atomic_read(&video->stopping) && video->ops &&
        video->ops->on_target_bitrate_changed) {
        video->ops->on_target_bitrate_changed(video->ctx, target_bps);
    }
#else
    (void)video;
    (void)target_bps;
#endif
}
