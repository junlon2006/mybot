/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_VIDEO_H_
#define MYBOT_VIDEO_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------
 * Platform video source operations (hook interface)
 *
 * The platform owns camera capture and JPEG/H.264/H.265 encoding. The SDK
 * forwards complete encoded frames to the active Agora RTC connection; it
 * does not encode, decode, queue, or receive video frames.
 * ---------------------------------------------------------- */

/**
 * Encoded video formats accepted by the SDK.
 *
 * The platform must encode the camera input before invoking the frame handler.
 * These values are MyBot values and are mapped internally to RTSA data types.
 */
typedef enum {
    /** A complete JPEG image. RTSA treats it as a key frame. */
    MYBOT_VIDEO_CODEC_JPEG = 0,
    /** An Annex-B H.264 access unit. */
    MYBOT_VIDEO_CODEC_H264,
    /** An Annex-B H.265 access unit. */
    MYBOT_VIDEO_CODEC_H265,
} mybot_video_codec_t;

/**
 * One complete encoded frame supplied by the platform encoder.
 *
 * A frame is one complete access unit. The SDK does not split an access unit
 * across callbacks, perform encoding, or retain this structure.
 */
typedef struct {
    /**
     * Encoded frame bytes, borrowed until the frame handler returns.
     * H.264/H.265 data must use the complete Annex-B access-unit format
     * expected by the RTSA packetizer; JPEG data must be a complete image.
     *
     * @note The platform must not reuse or modify this memory until the frame
     *       handler returns.
     */
    const void *data;
    /** Number of encoded bytes; must be non-zero and within the SDK limit. */
    size_t len;
    /** Encoded format: JPEG, H.264, or H.265. */
    mybot_video_codec_t codec;
} mybot_video_frame_t;

/**
 * Platform-to-SDK encoded frame callback.
 *
 * @param frame     encoded frame; borrowed for the duration of this call
 * @param user_data opaque context supplied by the SDK at init() time
 * @return 0 when the frame was accepted by RTSA, -1 when it was rejected or dropped
 *
 * @note Called from the platform encoder task, never an ISR. The callback may
 *       synchronously wait for the RTC worker while RTSA packetizes the frame.
 *       RTSA copies the payload before its send call returns.
 */
typedef int (*mybot_video_frame_handler_t)(const mybot_video_frame_t *frame, void *user_data);

/**
 * Video capture and encoding operations.
 *
 * `min_bps`, `max_bps`, `init`, `start`, `stop`, `on_target_bitrate_changed`,
 * and `destroy` are required when `MYBOT_ENABLE_VIDEO=ON`. The operations table
 * and all objects referenced by it must remain valid until destroy() returns.
 */
typedef struct {
    /**
     * Minimum video bitrate accepted by the platform encoder, in bits per second.
     * RTSA accepts zero for this lower bound, but max_bps must be non-zero.
     */
    uint32_t min_bps;

    /**
     * Maximum video bitrate accepted by the platform encoder, in bits per second.
     * Must be greater than or equal to min_bps.
     */
    uint32_t max_bps;

    /**
     * Allocate and initialize the encoder source without starting capture.
     *
     * @param ctx      [out] encoder context handle
     * @param handler  callback used to submit encoded frames
     * @param user_data opaque value forwarded to handler()
     * @return 0 on success, -1 on error
     *
     * @note The bitrate fields above are platform-owned encoder capabilities. The
     *       SDK passes them to agora_rtc_set_bwe_param() after creating the RTC
     *       connection and uses their midpoint as start_bps. Do not call RTC APIs
     *       from this callback. The handler and user_data remain valid until
     *       destroy() returns; no frame may be emitted before start().
     */
    int (*init)(void **ctx, mybot_video_frame_handler_t handler, void *user_data);

    /**
     * Start the capture and encoding stream.
     *
     * @param ctx encoder context from init()
     * @return 0 on success, -1 on error
     *
     * @note Called after RTC reports `CONNECTED`. Return promptly and do not
     *       invoke handler() synchronously; emit frames from the encoder task
     *       after this call returns.
     */
    int (*start)(void *ctx);

    /**
     * Stop the capture and encoding stream.
     *
     * @param ctx encoder context from init()
     * @return 0 when fully stopped, -1 while resources must be retained
     *
     * @note Stop producing frames and wait for every in-flight handler to
     *       return. No handler may run after stop() returns. The SDK calls
     *       this before leaving RTC and may retry after an error; destroy()
     *       is not called until stop() succeeds.
     */
    int (*stop)(void *ctx);

    /**
     * Request one key frame from the encoder.
     *
     * Optional; JPEG sources may leave this callback NULL.
     *
     * @param ctx encoder context from init()
     *
     * @note For H.264/H.265, schedule key-frame generation asynchronously.
     *       Return promptly and do not call back into the SDK.
     */
    void (*on_key_frame_request)(void *ctx);

    /**
     * Apply the current target video bitrate in bits per second.
     *
     * @param ctx       encoder context from init()
     * @param target_bps target bitrate reported by RTSA
     *
     * @note RTSA invokes this callback when its bandwidth estimate changes.
     *       The SDK forwards it on the RTC worker. Clamp the value to the
     *       encoder's limits, apply it without blocking, and return promptly.
     *       The SDK does not retry this callback.
     */
    void (*on_target_bitrate_changed)(void *ctx, uint32_t target_bps);

    /**
     * Destroy and release the encoder source.
     *
     * @param ctx encoder context from init()
     *
     * @note Called only after stop() completes successfully. No frame handler
     *       may be running when this callback is entered.
     */
    void (*destroy)(void *ctx);
} mybot_video_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_VIDEO_H_ */
