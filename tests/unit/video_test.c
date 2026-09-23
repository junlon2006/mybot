/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_video_internal.h"
#include "mybot_platform_registry.h"
#include "mybot_agora_rtc.h"

#include <api/aosl.h>
#include <hal/aosl_hal_time.h>

#include <assert.h>
#include <pthread.h>
#include <stdio.h>

static int s_context;
static int s_init_result;
static int s_start_result;
static int s_stop_result;
static int s_send_result;
static int s_destroy_calls;
static int s_key_requests;
static uint32_t s_target_bps;
static mybot_video_frame_handler_t s_handler;
static void *s_user_data;
static pthread_t s_encoder_thread;
static bool s_encoder_running;
static aosl_atomic_t s_block_send;
static aosl_atomic_t s_send_entered;
static aosl_atomic_t s_release_send;
static aosl_atomic_t s_stop_entered;
static aosl_atomic_t s_stop_finished;
static aosl_atomic_t s_send_calls;
static const unsigned char s_jpeg[] = {0xff, 0xd8, 0xff, 0xd9};
static const mybot_video_frame_t s_frame = {
    .data = s_jpeg, .len = sizeof(s_jpeg), .codec = MYBOT_VIDEO_CODEC_JPEG};

static bool wait_for_flag(aosl_atomic_t *flag) {
    for (int elapsed = 0; elapsed < 2000; ++elapsed) {
        if (aosl_atomic_read(flag)) {
            return true;
        }
        aosl_hal_msleep(1);
    }
    return false;
}

static int video_init(void **ctx, mybot_video_frame_handler_t handler, void *user_data) {
    assert(handler != NULL && user_data != NULL);
    if (s_init_result < 0) {
        return s_init_result;
    }
    *ctx = &s_context;
    s_handler = handler;
    s_user_data = user_data;
    return 0;
}

static int video_start(void *ctx) {
    assert(ctx == &s_context);
    return s_start_result;
}

static int video_stop(void *ctx) {
    assert(ctx == &s_context);
    aosl_atomic_set(&s_stop_entered, true);
    if (s_stop_result < 0) {
        return s_stop_result;
    }
    /* The platform contract requires stop to drain its frame callbacks. */
    if (s_encoder_running) {
        assert(pthread_join(s_encoder_thread, NULL) == 0);
        s_encoder_running = false;
    }
    return 0;
}

static void video_destroy(void *ctx) {
    assert(ctx == &s_context);
    assert(!s_encoder_running);
    s_destroy_calls++;
}

static void on_key_frame_request(void *ctx) {
    assert(ctx == &s_context);
    s_key_requests++;
}

static void on_target_bitrate_changed(void *ctx, uint32_t bps) {
    assert(ctx == &s_context);
    s_target_bps = bps;
}

static mybot_video_ops_t s_ops = {
    .min_bps = 32000,
    .max_bps = 256000,
    .init = video_init,
    .start = video_start,
    .stop = video_stop,
    .on_key_frame_request = on_key_frame_request,
    .on_target_bitrate_changed = on_target_bitrate_changed,
    .destroy = video_destroy,
};
static mybot_platform_descriptor_t s_platform = {.video = &s_ops};

const mybot_platform_descriptor_t *mybot_platform_registry_get(void) {
    return &s_platform;
}

int mybot_agora_rtc_send_video(const mybot_video_frame_t *frame) {
    assert(frame == &s_frame && frame->data == s_jpeg && frame->len == sizeof(s_jpeg));
    aosl_atomic_inc(&s_send_calls);
    if (aosl_atomic_read(&s_block_send)) {
        aosl_atomic_set(&s_send_entered, true);
        assert(wait_for_flag(&s_release_send));
    }
    return s_send_result;
}

static void *encode_one_frame(void *arg) {
    (void)arg;
    assert(s_handler(&s_frame, s_user_data) == 0);
    return NULL;
}

static void *stop_video(void *arg) {
    assert(mybot_video_stop(arg) == 0);
    aosl_atomic_set(&s_stop_finished, true);
    return NULL;
}

int main(void) {
    aosl_ctor();
    mybot_video_t video = {0};
    s_init_result = -1;
    mybot_video_init(&video);
    assert(!video.initialized);
    assert(mybot_video_start(&video) < 0);
    assert(mybot_video_destroy(&video) == 0 && s_destroy_calls == 0);

    s_init_result = 0;
    mybot_video_init(&video);
    assert(video.initialized);
    assert(video.min_bps == s_ops.min_bps && video.max_bps == s_ops.max_bps);
    assert(s_handler(&s_frame, s_user_data) < 0);
    assert(aosl_atomic_read(&s_send_calls) == 0);

    s_start_result = -1;
    assert(mybot_video_start(&video) < 0);
    assert(s_handler(&s_frame, s_user_data) < 0);
    mybot_video_request_key_frame(&video);
    assert(s_key_requests == 0);
    s_start_result = 0;
    assert(mybot_video_start(&video) == 0);
    mybot_video_set_target_bitrate(&video, 128000);
    mybot_video_request_key_frame(&video);
    assert(s_target_bps == 128000 && s_key_requests == 1);

    s_send_result = -1;
    assert(s_handler(&s_frame, s_user_data) == -1);
    s_send_result = 0;
    assert(s_handler(&s_frame, s_user_data) == 0);

    /* A failed stop retains the encoder and rejects new frames until retry. */
    s_stop_result = -1;
    assert(mybot_video_stop(&video) < 0);
    assert(s_handler(&s_frame, s_user_data) < 0);
    assert(mybot_video_start(&video) < 0);
    assert(mybot_video_destroy(&video) < 0 && s_destroy_calls == 0);
    mybot_video_set_target_bitrate(&video, 64000);
    mybot_video_request_key_frame(&video);
    assert(s_target_bps == 128000 && s_key_requests == 1);
    s_stop_result = 0;
    assert(mybot_video_stop(&video) == 0);
    assert(mybot_video_start(&video) == 0);

    /* Stop while the encoder is inside the SDK's synchronous send handler. */
    aosl_atomic_set(&s_stop_entered, false);
    aosl_atomic_set(&s_block_send, true);
    s_encoder_running = true;
    assert(pthread_create(&s_encoder_thread, NULL, encode_one_frame, NULL) == 0);
    assert(wait_for_flag(&s_send_entered));
    pthread_t stop_thread;
    assert(pthread_create(&stop_thread, NULL, stop_video, &video) == 0);
    assert(wait_for_flag(&s_stop_entered));
    assert(!aosl_atomic_read(&s_stop_finished));
    intptr_t sends_before_stop = aosl_atomic_read(&s_send_calls);
    assert(s_handler(&s_frame, s_user_data) < 0);
    assert(aosl_atomic_read(&s_send_calls) == sends_before_stop);
    aosl_atomic_set(&s_release_send, true);
    assert(pthread_join(stop_thread, NULL) == 0);
    assert(aosl_atomic_read(&s_stop_finished));
    assert(mybot_video_destroy(&video) == 0 && s_destroy_calls == 1);
    assert(mybot_video_destroy(&video) == 0 && s_destroy_calls == 1);

    /* A new session may omit the optional key-frame callback. */
    s_ops.on_key_frame_request = NULL;
    mybot_video_init(&video);
    assert(mybot_video_start(&video) == 0);
    mybot_video_request_key_frame(&video);
    assert(s_key_requests == 1);
    assert(mybot_video_stop(&video) == 0);
    assert(mybot_video_destroy(&video) == 0 && s_destroy_calls == 2);
    aosl_dtor();
    puts("video_test: ok");
    return 0;
}
