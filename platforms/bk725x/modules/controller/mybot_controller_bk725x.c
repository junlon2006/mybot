/* SPDX-License-Identifier: Apache-2.0 */
/*
 * BK725x application controller.
 *
 * The SDK owns the application state machine.  Following the BK7259
 * cross-platform port, this controller keeps a small poll loop at the
 * application boundary: it owns the product modules (display, buttons,
 * STA worker, APSTA provisioning, shared playback) and only starts and
 * stops the SDK, logging the SDK's state-view at observable edges.
 */
#include <bk725x_platform_adapters.h>
#include <common/bk_err.h>
#include <components/bk_uid.h>
#include "mybot_platform_log.h"
#include <mybot/mybot.h>
#include <mybot/mybot_version.h>
#include "mybot_language.h"
#include <mybot_audio_shared_bk725x.h>
#include <mybot_button.h>
#include <mybot_connectivity.h>
#if CONFIG_MYBOT_DEBUG_CPU
#include <mybot_cpu_monitor_bk725x.h>
#endif
#include <mybot_display.h>
#include <mybot_event.h>
#include <mybot_key_dispatcher.h>
#include <mybot_network.h>
#include <mybot_prompt_player_bk725x.h>
#include <mybot_provisioning.h>
#include <mybot_sdcard_msc_bk725x.h>
#include <mybot_controller.h>
#include <mbedtls/md5.h>
#include <os/os.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define CONTROLLER_THREAD_PRIORITY 2
#define CONTROLLER_THREAD_STACK_SIZE 4096
#define CONTROLLER_POLL_MS 100

#define TAG "mybot_ctrl"

extern volatile const char build_version[];

typedef struct {
    mybot_config_t config;
    bool event_initialized;
    bool button_initialized;
#if CONFIG_MYBOT_DEBUG_CPU
    bool cpu_monitor_started;
#endif
    bool display_initialized;
    bool provisioning_active;
    bool network_started;
    bool network_connected;
    bool sdk_active;
    bool provision_requested;
    bool network_success_prompt_pending;
} app_runtime_t;

static beken_thread_t s_controller_thread;

static void show_display_screen(const app_runtime_t *runtime, mybot_display_screen_t screen) {
    if (runtime->display_initialized && mybot_display_show_screen(screen, 0) < 0) {
        MYBOT_LOGW(TAG, "failed to render display screen=%d", (int)screen);
    }
}

static const char *controller_state_name(mybot_state_t state) {
    switch (state) {
    case MYBOT_STATE_STOPPED:
        return "stopped";
    case MYBOT_STATE_WIFI_PROVISIONING:
        return "wifi_provisioning";
    case MYBOT_STATE_STARTING_SERVICES:
        return "starting_services";
    case MYBOT_STATE_READY:
        return "ready";
    case MYBOT_STATE_WIFI_DISCONNECTED:
        return "wifi_disconnected";
    case MYBOT_STATE_FAILED:
        return "failed";
    case MYBOT_STATE_STOPPING:
        return "stopping";
    case MYBOT_STATE_IN_CONVERSATION:
        return "in_conversation";
    default:
        break;
    }
    return "unknown";
}

/* The SDK owns the state machine: poll its atomic view and emit one log
 * record per observed edge, exactly like the BK7259 application loop. */
static void log_state_transition(mybot_state_t *last_state, bool *valid) {
    mybot_state_t current = mybot_get_state();

    if (!*valid) {
        MYBOT_LOGI(TAG, "runtime state=%s(%d)", controller_state_name(current),
                (int)current);
        *last_state = current;
        *valid = true;
        return;
    }
    if (current != *last_state) {
        MYBOT_LOGI(TAG, "runtime state transition: %s(%d) -> %s(%d)",
                controller_state_name(*last_state), (int)*last_state,
                controller_state_name(current), (int)current);
        *last_state = current;
    }
}

static int build_device_config(mybot_config_t *config) {
    static const char hex[] = "0123456789abcdef";
    unsigned char uid[32] = {0};
    unsigned char digest[16];

    if (!config || bk_uid_get_data(uid) != BK_OK) {
        MYBOT_LOGE(TAG, "failed to read device UID");
        return -1;
    }
    if (mbedtls_md5(uid, sizeof(uid), digest) != 0) {
        MYBOT_LOGE(TAG, "failed to hash device UID");
        return -1;
    }

    *config = (mybot_config_t){0};
    for (size_t i = 0; i < 16; ++i) {
        config->device_id[i * 2] = hex[digest[i] >> 4];
        config->device_id[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    config->device_id[32] = '\0';

    int length = snprintf(config->server_base, sizeof(config->server_base), "%s",
                          MYBOT_SERVER_BASE);
    if (length < 0 || (size_t)length >= sizeof(config->server_base)) {
        MYBOT_LOGE(TAG, "configured server URL is too long");
        return -1;
    }
    snprintf(config->firmware_ver, sizeof(config->firmware_ver), "%s",
             mybot_version_string());
    snprintf(config->hw_model, sizeof(config->hw_model), "bk725x");
    return 0;
}

static void publish_connectivity(mybot_connectivity_event_t event) {
    if (mybot_connectivity_publish(event) < 0) {
        MYBOT_LOGE(TAG, "failed to publish connectivity event=%d", (int)event);
    }
}

/* Publish connectivity transitions to the SDK Wi-Fi adapter when the STA
 * runtime reports a usable IPv4 connection or loses it. */
static void update_network_state(app_runtime_t *runtime) {
    bool connected = mybot_network_is_connected();

    if (connected == runtime->network_connected) {
        return;
    }
    runtime->network_connected = connected;
    publish_connectivity(connected ? MYBOT_CONNECTIVITY_CONNECTED
                                   : MYBOT_CONNECTIVITY_DISCONNECTED);
    if (connected) {
        MYBOT_LOGI(TAG, "network connected");
        if (!runtime->sdk_active) {
            show_display_screen(runtime, MYBOT_DISPLAY_SCREEN_STARTING_SERVICES);
        }
    } else {
        MYBOT_LOGW(TAG, "network disconnected");
    }
}

static int start_network(app_runtime_t *runtime) {
    bool configured = false;

    if (mybot_network_is_configured(&configured) < 0) {
        MYBOT_LOGE(TAG, "failed to inspect saved Wi-Fi credentials");
        return -1;
    }
    if (!configured) {
        runtime->provision_requested = true;
        return 0;
    }

    MYBOT_LOGI(TAG, "starting normal STA networking");
    runtime->network_connected = false;
    publish_connectivity(MYBOT_CONNECTIVITY_DISCONNECTED);
    if (mybot_network_start() < 0) {
        MYBOT_LOGE(TAG, "normal STA networking start failed");
        return -1;
    }
    runtime->network_started = true;
    show_display_screen(runtime, MYBOT_DISPLAY_SCREEN_WIFI_DISCONNECTED);
    return 0;
}

static void stop_network(app_runtime_t *runtime) {
    if (!runtime->network_started) {
        return;
    }
    MYBOT_LOGI(TAG, "stopping normal STA networking");
    runtime->network_started = false;
    runtime->network_connected = false;
    publish_connectivity(MYBOT_CONNECTIVITY_DISCONNECTED);
    if (mybot_network_stop() < 0) {
        MYBOT_LOGE(TAG, "normal STA networking stop incomplete");
    }
}

static int start_provisioning(app_runtime_t *runtime) {
    MYBOT_LOGI(TAG, "starting APSTA provisioning");
    if (mybot_provisioning_start(runtime->config.device_id) < 0) {
        MYBOT_LOGE(TAG, "APSTA provisioning start failed");
        return -1;
    }
    runtime->provisioning_active = true;
    show_display_screen(runtime, MYBOT_DISPLAY_SCREEN_WIFI_PROVISIONING);

    /* Start the shared playback pipeline before the prompt player so the
     * prompt writes through the same audio path the SDK will use later. */
    if (mybot_audio_bk725x_shared_playback_start() < 0) {
        MYBOT_LOGW(TAG, "shared playback start failed, prompt may be silent");
    }
    if (mybot_prompt_player_bk725x_play_provisioning() < 0) {
        MYBOT_LOGW(TAG, "failed to start provisioning prompt");
    }
    return 0;
}

static void stop_provisioning(app_runtime_t *runtime) {
    if (!runtime->provisioning_active) {
        return;
    }
    mybot_prompt_player_bk725x_stop();
    runtime->provisioning_active = false;
    if (mybot_provisioning_stop() < 0) {
        MYBOT_LOGE(TAG, "APSTA provisioning stop incomplete");
    }
}

static int start_sdk(app_runtime_t *runtime) {
    if (runtime->sdk_active || !runtime->network_connected) {
        return 0;
    }
    mybot_prompt_player_bk725x_stop();
    if (mybot_audio_bk725x_shared_playback_start() < 0) {
        MYBOT_LOGE(TAG, "shared playback start failed");
        return -1;
    }
    if (runtime->network_success_prompt_pending) {
        if (mybot_prompt_player_bk725x_play_success_sync() < 0) {
            MYBOT_LOGW(TAG, "failed to play provisioning success prompt");
        }
        runtime->network_success_prompt_pending = false;
    }
    if (bk725x_platform_adapters_register() < 0) {
        return -1;
    }
    /* The SDK Wi-Fi adapter consumes this connected snapshot during
     * mybot_start() and immediately forwards it to SDK startup. */
    publish_connectivity(MYBOT_CONNECTIVITY_CONNECTED);
    MYBOT_LOGI(TAG, "starting mybot SDK");
    if (mybot_start(&runtime->config) < 0) {
        MYBOT_LOGE(TAG, "mybot SDK start failed");
        return -1;
    }

    runtime->sdk_active = true;
    MYBOT_LOGI(TAG, "mybot SDK started");
    return 0;
}

static void stop_sdk(app_runtime_t *runtime) {
    if (!runtime->sdk_active) {
        return;
    }
    MYBOT_LOGI(TAG, "stopping mybot SDK");
    mybot_stop();
    runtime->sdk_active = false;
}

/* Forward semantic key actions to the SDK key adapter.  The dispatcher has no
 * subscriber while the SDK is stopped, so ignore presses like the BK7259 key
 * callback does. */
static void forward_button_event(const app_runtime_t *runtime, mybot_event_type_t type) {
    mybot_key_action_t action;
    mybot_state_t state;

    if (!runtime->sdk_active) {
        return;
    }

    switch (type) {
    case MYBOT_EVENT_BUTTON_VOLUME_UP:
        action = MYBOT_KEY_ACTION_VOLUME_UP;
        break;
    case MYBOT_EVENT_BUTTON_VOLUME_DOWN:
        action = MYBOT_KEY_ACTION_VOLUME_DOWN;
        break;
    case MYBOT_EVENT_BUTTON_CONVERSATION_TOGGLE:
        state = mybot_get_state();
        if (state == MYBOT_STATE_READY) {
            action = MYBOT_KEY_ACTION_CONVERSATION_START;
        } else if (state == MYBOT_STATE_IN_CONVERSATION) {
            action = MYBOT_KEY_ACTION_CONVERSATION_STOP;
        } else {
            return;
        }
        break;
    default:
        return;
    }

    MYBOT_LOGI(TAG, "key action=%d", (int)action);
    (void)mybot_key_dispatcher_publish(action);
}

static void handle_button_event(app_runtime_t *runtime, const mybot_event_t *event) {
    if (event->type == MYBOT_EVENT_BUTTON_PROVISIONING_REQUEST) {
        if (runtime->provisioning_active) {
            return;
        }
        MYBOT_LOGI(TAG, "provisioning requested");
        runtime->provision_requested = true;
        return;
    }
    forward_button_event(runtime, event->type);
}

static void controller_wait_for_button(app_runtime_t *runtime) {
    mybot_event_t event;
    if (mybot_event_wait(&event, CONTROLLER_POLL_MS) == 0) {
        handle_button_event(runtime, &event);
    }
}

/* Wait for the STA worker to report a usable IPv4 connection.  A provisioning
 * request interrupts the wait; the caller restarts the top of the loop. */
static int wait_for_network(app_runtime_t *runtime) {
    while (!mybot_network_is_connected()) {
        if (runtime->provision_requested) {
            return 0;
        }
        controller_wait_for_button(runtime);
    }
    update_network_state(runtime);
    return 0;
}

/* Wait for the APSTA worker's authoritative result, keeping buttons live. */
static int wait_for_provisioning(app_runtime_t *runtime) {
    for (;;) {
        mybot_provisioning_state_t state = mybot_provisioning_get_state();

        if (state == MYBOT_PROVISIONING_STATE_COMPLETED) {
            MYBOT_LOGI(TAG, "APSTA provisioning completed");
            stop_provisioning(runtime);
            return 0;
        }
        if (state == MYBOT_PROVISIONING_STATE_FAILED) {
            MYBOT_LOGE(TAG, "APSTA provisioning failed");
            stop_provisioning(runtime);
            return -1;
        }
        controller_wait_for_button(runtime);
    }
}

/* Product control loop.  Modeled on the BK7259 mybot_run(): the SDK is the
 * only state machine owner; this loop orchestrates network/provisioning and
 * SDK sessions and exits when the SDK fails or stops unexpectedly. */
static int controller_run(app_runtime_t *runtime) {
    bool configured = false;
    mybot_state_t last_state = MYBOT_STATE_STOPPED;
    bool state_valid = false;

    if (mybot_network_is_configured(&configured) < 0) {
        MYBOT_LOGE(TAG, "failed to inspect saved Wi-Fi credentials");
        return -1;
    }
    MYBOT_LOGI(TAG, "Wi-Fi provisioning state: %s", configured ? "configured" : "new device");
    runtime->provision_requested = !configured;

    for (;;) {
        if (runtime->provision_requested) {
            runtime->provision_requested = false;
            stop_sdk(runtime);
            stop_network(runtime);
            if (start_provisioning(runtime) < 0 || wait_for_provisioning(runtime) < 0) {
                return -1;
            }
            runtime->network_success_prompt_pending = true;
            /* Save credentials are now present: restart the top of the loop
             * so the STA worker is started before waiting for connectivity. */
            continue;
        } else if (!runtime->network_started) {
            if (start_network(runtime) < 0) {
                return -1;
            }
        }

        if (wait_for_network(runtime) < 0) {
            return -1;
        }
        if (runtime->provision_requested) {
            continue;
        }

        if (!runtime->sdk_active) {
            if (start_sdk(runtime) < 0) {
                return -1;
            }
            log_state_transition(&last_state, &state_valid);
        }

        bool sdk_failed = false;
        while (runtime->sdk_active && mybot_is_running()) {
            controller_wait_for_button(runtime);
            update_network_state(runtime);
            log_state_transition(&last_state, &state_valid);
            if (mybot_get_state() == MYBOT_STATE_FAILED) {
                MYBOT_LOGE(TAG, "SDK entered the failed state");
                sdk_failed = true;
                break;
            }
            if (runtime->provision_requested) {
                break;
            }
        }
        log_state_transition(&last_state, &state_valid);

        if (runtime->provision_requested) {
            continue;
        }

        MYBOT_LOGW(TAG, "SDK stopped, state=%d", (int)mybot_get_state());
        stop_sdk(runtime);
        return sdk_failed ? -1 : 0;
    }
}

static void controller_cleanup(app_runtime_t *runtime) {
    mybot_prompt_player_bk725x_stop();
    /* Keep the shared pipeline alive while the SDK tears down its playback
     * ops, then release the audio power vote. */
    mybot_audio_bk725x_shared_playback_start();
    stop_sdk(runtime);
    mybot_audio_bk725x_shared_playback_stop();
    stop_network(runtime);
    stop_provisioning(runtime);
    if (runtime->button_initialized) {
        mybot_button_deinit();
        runtime->button_initialized = false;
    }
    if (runtime->event_initialized) {
        mybot_event_deinit();
        runtime->event_initialized = false;
    }
#if CONFIG_MYBOT_DEBUG_CPU
    if (runtime->cpu_monitor_started) {
        mybot_cpu_monitor_bk725x_stop();
        runtime->cpu_monitor_started = false;
    }
#endif
    if (runtime->display_initialized) {
        mybot_display_deinit();
        runtime->display_initialized = false;
    }
}

static void controller_thread(void *arg) {
    app_runtime_t runtime = {0};
    int result;

    (void)arg;

    MYBOT_LOGI(TAG, "initializing mybot application");
    if (build_device_config(&runtime.config) < 0) {
        goto cleanup;
    }
    MYBOT_LOGI(TAG, "device=%s server=%s", runtime.config.device_id,
            runtime.config.server_base);

#if CONFIG_MYBOT_DEBUG_CPU
    if (mybot_cpu_monitor_bk725x_start() < 0) {
        MYBOT_LOGE(TAG, "CPU monitor start failed");
        goto cleanup;
    }
    runtime.cpu_monitor_started = true;
#endif

    if (mybot_display_init() < 0) {
        MYBOT_LOGE(TAG, "dual display initialization failed");
        goto cleanup;
    }
    runtime.display_initialized = true;
    show_display_screen(&runtime, MYBOT_DISPLAY_SCREEN_STARTING);

#if CONFIG_USBD_MSC
    if (mybot_sdcard_msc_bk725x_init() < 0) {
        MYBOT_LOGW(TAG, "SD card USB access is unavailable");
    }
#endif

    if (mybot_event_init() < 0) {
        goto cleanup;
    }
    runtime.event_initialized = true;
    if (mybot_connectivity_prepare() < 0 || mybot_key_dispatcher_prepare() < 0) {
        MYBOT_LOGE(TAG, "event dispatcher initialization failed");
        goto cleanup;
    }
    if (mybot_button_init() < 0) {
        goto cleanup;
    }
    runtime.button_initialized = true;

    MYBOT_LOGI(TAG, "application controller ready");
    result = controller_run(&runtime);
    MYBOT_LOGI(TAG, "application controller exited, result=%d", result);

cleanup:
    controller_cleanup(&runtime);
    s_controller_thread = NULL;
    rtos_delete_thread(NULL);
}

int mybot_controller_start(void) {
    aosl_set_log_level(AOSL_LOG_NOTICE);

    MYBOT_LOGI(TAG, "mybot version: %s, build time: %s", mybot_version_string(),
            (const char *)build_version);

    if (s_controller_thread) {
        MYBOT_LOGI(TAG, "application controller already started");
        return 0;
    }
    MYBOT_LOGI(TAG, "starting application controller");

    bk_err_t result = rtos_create_psram_thread(
        &s_controller_thread, CONTROLLER_THREAD_PRIORITY, "mybot_ctrl", controller_thread,
        CONTROLLER_THREAD_STACK_SIZE, NULL);
    if (result != BK_OK) {
        s_controller_thread = NULL;
        MYBOT_LOGE(TAG, "failed to create controller thread: %d", result);
        return -1;
    }
    return 0;
}
