/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_H_
#define MYBOT_H_

#include <stdint.h>
#include <stdbool.h>
#include <mybot/mybot_errors.h>
#include <mybot/mybot_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SDK configuration supplied by the host application.
 *
 * All string fields must be NUL-terminated within their fixed-size buffers;
 * mybot_start() validates this and rejects the configuration otherwise.
 * Empty (zero-length) strings are allowed only where noted.
 */
typedef struct {
    /** Device-service base URL. Must be a non-empty "https://" URL (or
     *  "http://" in an explicitly insecure development build). */
    char server_base[128];
    /** Unique device identifier reported to the device service, e.g.
     *  "AG-A1B2C3". Must be non-empty. */
    char device_id[64];
    /** Firmware version string reported to the device service. Optional;
     *  may be empty. */
    char firmware_ver[32];
    /** Hardware model string reported to the device service. Optional;
     *  may be empty. */
    char hw_model[32];
} mybot_config_t;

/**
 * Application-level lifecycle state, returned by mybot_get_state().
 *
 * Describes the startup / runtime state of the whole SDK application instance,
 * including Wi-Fi provisioning, service bring-up, conversation activity,
 * connectivity, and shutdown.
 */
typedef enum {
    /** Not started, or fully stopped. Entered at the end of mybot_stop()
     *  after all worker threads and devices are released; also the value
     *  reported before mybot_start(). */
    MYBOT_STATE_STOPPED = 0,
    /** The platform Wi-Fi provisioning/connection workflow is in progress.
     *  mybot_start() is non-blocking and returns after starting that workflow;
     *  this state lasts until MYBOT_WIFI_EVENT_STA_CONNECTED is reported. */
    MYBOT_STATE_WIFI_PROVISIONING,
    /** Wi-Fi is connected and the remaining startup services (KV storage,
     *  keys, audio capture/playback and the device-service state machine) are
     *  being initialized asynchronously. RTC is initialized on demand when a
     *  conversation starts. A failure here transitions to MYBOT_STATE_FAILED. */
    MYBOT_STATE_STARTING_SERVICES,
    /** All startup services are up and the device is ready to start a
     *  conversation or re-pair. */
    MYBOT_STATE_READY,
    /** Runtime Wi-Fi link was lost (or failed after provisioning);
     *  device-service traffic is paused and any active RTC conversation is
     *  ended locally. Returns to MYBOT_STATE_READY on reconnect. */
    MYBOT_STATE_WIFI_DISCONNECTED,
    /** Unrecoverable failure: Wi-Fi provisioning, service bring-up, or a
     *  runtime event queue failure. The application should report the error
     *  and call mybot_stop(). */
    MYBOT_STATE_FAILED,
    /** mybot_stop() is in progress: worker threads, audio devices, TLS and
     *  RTC resources are being torn down. Ends in MYBOT_STATE_STOPPED. */
    MYBOT_STATE_STOPPING,
    /** The device service has accepted a conversation. This includes RTC
     *  setup, the active session, and normal conversation teardown until the
     *  device lifecycle returns to MYBOT_STATE_READY. If runtime connectivity
     *  is lost, MYBOT_STATE_WIFI_DISCONNECTED takes precedence while offline. */
    MYBOT_STATE_IN_CONVERSATION = 7,
} mybot_state_t;

/**
 * @brief Initialize and start the application.
 *
 * Non-blocking: starts the registered platform Wi-Fi workflow and returns
 * immediately. The remaining startup services (KV storage, keys, audio and the
 * device-service state machine) are initialized asynchronously on the startup
 * worker once MYBOT_WIFI_EVENT_STA_CONNECTED is reported. RTC is
 * initialized later, when a conversation starts.
 *
 * Product platforms should normally use the recommended APSTA provisioning
 * workflow to keep onboarding and connection behavior consistent across
 * products; see mybot_wifi_ops_t. The generic platform contract also supports
 * development environments whose host already manages the network connection.
 *
 * Preconditions:
 * - Register one complete mybot_platform_descriptor_t first. Wi-Fi, KV, key, audio capture and
 *   playback are required; HTTPS and wake words become required when selected by the build and
 *   configuration.
 * - With MYBOT_ENABLE_HTTPS=ON, a "https://" server requires a TLS transport in the registered
 *   platform descriptor; plain "http://" is rejected
 *   unless MYBOT_ALLOW_INSECURE_HTTP=ON is set for development builds.
 *
 * The configuration is validated (non-NULL cfg, NUL-terminated strings,
 * non-empty server_base / device_id, supported URL scheme). Calling
 * mybot_start() while the application is already active returns -1.
 *
 * @param cfg application configuration; must stay valid only for the duration
 *            of the call (it is copied).
 * @return 0 on success, -1 on error. On error, all partially initialized
 *         resources have already been released and the application is stopped.
 *
 * @note The caller must call mybot_stop() before exiting the process, and
 *       may call mybot_start() again after mybot_stop() returns.
 * @note mybot_start() acquires one application reference to the process-wide
 *       AOSL runtime. The matching reference is released by mybot_stop();
 *       applications that use AOSL directly must maintain their own ctor/dtor
 *       pair for the duration of that use.
 * @note Call from the main application thread, not from a platform event
 *       callback.
 * @note Calls to mybot_start() and mybot_stop() are serialized internally. A
 *       concurrent mybot_stop() waits for startup to finish; a concurrent
 *       mybot_start() observes the completed active state and returns -1.
 */
MYBOT_API int mybot_start(const mybot_config_t *cfg);

/**
 * @brief Check whether the application instance is still running.
 *
 * @return true from a successful mybot_start() until the application stops or
 *         an internal exit event clears the running flag; false otherwise (also when
 *         not started, after a failed start, or after stopping).
 *
 * @note Thread-safe (atomic read). The main loop should poll this and call
 *       mybot_stop() once it returns false.
 */
MYBOT_API bool mybot_is_running(void);

/**
 * @brief Return the current application lifecycle state.
 *
 * @return One of the mybot_state_t values. During a device-service
 *         conversation with usable connectivity, returns
 *         MYBOT_STATE_IN_CONVERSATION until normal teardown completes. If
 *         runtime connectivity is lost, returns MYBOT_STATE_WIFI_DISCONNECTED
 *         until reconnect.
 *
 * @note Thread-safe (atomic read).
 * @see mybot_state_t
 */
MYBOT_API mybot_state_t mybot_get_state(void);

/**
 * @brief Stop the application and release all resources.
 *
 * Signals every worker to stop, waits for all worker threads to exit, and
 * releases audio devices, TLS, Wi-Fi, LCD and RTC resources.
 *
 * Idempotent: safe to call when the application is not running, after a
 * failed mybot_start(), or repeatedly. After it returns, the application
 * returns to MYBOT_STATE_STOPPED and may be started again.
 *
 * RTC shutdown releases the Agora SDK's independent AOSL reference first;
 * mybot releases its application reference last, after all application-owned
 * workers and buffers have been destroyed.
 *
 * @warning Blocks until shutdown completes, so it must be called from the
 *          main application thread — never from inside a platform event
 *          callback, an SDK worker thread, or a signal handler.
 * @note Calls concurrent with mybot_start() are serialized and wait for the
 *       startup attempt to finish before teardown begins.
 */
MYBOT_API void mybot_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_H_ */
