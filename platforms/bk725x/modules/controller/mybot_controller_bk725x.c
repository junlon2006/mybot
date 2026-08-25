/* SPDX-License-Identifier: Apache-2.0 */
#include <bk725x_platform_adapters.h>
#include <common/bk_err.h>
#include <components/bk_uid.h>
#include "mybot_platform_log.h"
#include <mybot/mybot.h>
#include <mybot/mybot_version.h>
#include "mybot_language.h"
#include <mybot_audio_shared_bk725x.h>
#include <mybot_audio_power_bk725x.h>
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
#define CONTROLLER_WAIT_MS 100
#define MYBOT_RESTART_DELAY_MS 5000

#define TAG "mybot_ctrl"

extern volatile const char build_version[];

typedef enum {
    APP_WIFI_IDLE = 0,
    APP_WIFI_NETWORK,
    APP_WIFI_PROVISIONING,
} app_wifi_mode_t;

typedef enum {
    CONTROLLER_STATE_WIFI_IDLE = 0,
    CONTROLLER_STATE_PROVISIONING,
    CONTROLLER_STATE_NETWORK_DISCONNECTED,
    CONTROLLER_STATE_NETWORK_CONNECTED_STOPPED,
    CONTROLLER_STATE_NETWORK_CONNECTED_ACTIVE,
    CONTROLLER_STATE_NETWORK_DISCONNECTED_ACTIVE,
    CONTROLLER_STATE_COUNT,
} controller_state_t;

#define CONTROLLER_STATE_FLAG(state) (1u << (state))

#define CONTROLLER_ALL_STATES \
    ((1u << CONTROLLER_STATE_COUNT) - 1u)

#define CONTROLLER_ACTIVE_STATES \
    (CONTROLLER_STATE_FLAG(CONTROLLER_STATE_NETWORK_CONNECTED_ACTIVE) | \
     CONTROLLER_STATE_FLAG(CONTROLLER_STATE_NETWORK_DISCONNECTED_ACTIVE))

#define CONTROLLER_NETWORK_STATES \
    (CONTROLLER_STATE_FLAG(CONTROLLER_STATE_NETWORK_DISCONNECTED) | \
     CONTROLLER_STATE_FLAG(CONTROLLER_STATE_NETWORK_CONNECTED_STOPPED) | \
     CONTROLLER_STATE_FLAG(CONTROLLER_STATE_NETWORK_CONNECTED_ACTIVE) | \
     CONTROLLER_STATE_FLAG(CONTROLLER_STATE_NETWORK_DISCONNECTED_ACTIVE))

#define CONTROLLER_NETWORK_STOPPED_STATES \
    (CONTROLLER_STATE_FLAG(CONTROLLER_STATE_NETWORK_DISCONNECTED) | \
     CONTROLLER_STATE_FLAG(CONTROLLER_STATE_NETWORK_CONNECTED_STOPPED))

#define CONTROLLER_PROVISIONING_STATES \
    (CONTROLLER_STATE_FLAG(CONTROLLER_STATE_PROVISIONING))

typedef enum {
    CONTROLLER_EVENT_NONE = 0,
    CONTROLLER_EVENT_VOLUME_UP,
    CONTROLLER_EVENT_VOLUME_DOWN,
    CONTROLLER_EVENT_CONVERSATION_TOGGLE,
    CONTROLLER_EVENT_PROVISIONING_REQUEST,
    CONTROLLER_EVENT_NETWORK_CONNECTED,
    CONTROLLER_EVENT_NETWORK_DISCONNECTED,
    CONTROLLER_EVENT_NETWORK_FAILED,
    CONTROLLER_EVENT_PROVISIONING_COMPLETED,
    CONTROLLER_EVENT_PROVISIONING_FAILED,
    CONTROLLER_EVENT_POLL_NETWORK_STATE,
    CONTROLLER_EVENT_POLL_PROVISIONING_STATE,
    CONTROLLER_EVENT_POLL_WIFI_RECONCILE,
    CONTROLLER_EVENT_POLL_MYBOT_RUNNING,
    CONTROLLER_EVENT_POLL_MYBOT_RESTART,
    CONTROLLER_EVENT_COUNT,
} controller_event_kind_t;

typedef struct {
    controller_event_kind_t kind;
    uint32_t now_ms;
    uint32_t source_generation;
} controller_event_t;

typedef struct {
    mybot_config_t config;
    /* Derived state, recomputed after every event and tick transition. */
    controller_state_t state;
    app_wifi_mode_t wifi_mode;
    bool event_initialized;
    bool button_initialized;
#if CONFIG_MYBOT_DEBUG_CPU
    bool cpu_monitor_started;
#endif
    bool display_initialized;
    bool audio_power_acquired;
    bool mybot_active;
    bool network_connected;
    bool provisioning_prompt_announced;
    uint32_t wifi_generation;
    uint32_t mybot_restart_at;
    uint32_t wifi_retry_at;
    app_wifi_mode_t desired_wifi_mode;
} app_runtime_t;

static beken_thread_t s_controller_thread;

static uint32_t controller_now_ms(void) {
    beken_time_t now = 0;

    (void)beken_time_get_time(&now);
    return now;
}

static void show_display_screen(const app_runtime_t *runtime, mybot_display_screen_t screen) {
    if (runtime->display_initialized && mybot_display_show_screen(screen) < 0) {
        MYBOT_LOGW(TAG, "failed to render display screen=%d", (int)screen);
    }
}

static bool controller_deadline_reached(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

static uint32_t controller_state_flag(controller_state_t state) {
    return CONTROLLER_STATE_FLAG(state);
}

static const char *controller_state_name(controller_state_t state) {
    switch (state) {
    case CONTROLLER_STATE_WIFI_IDLE:
        return "wifi_idle";
    case CONTROLLER_STATE_PROVISIONING:
        return "provisioning";
    case CONTROLLER_STATE_NETWORK_DISCONNECTED:
        return "network_disconnected";
    case CONTROLLER_STATE_NETWORK_CONNECTED_STOPPED:
        return "network_connected_stopped";
    case CONTROLLER_STATE_NETWORK_CONNECTED_ACTIVE:
        return "network_connected_active";
    case CONTROLLER_STATE_NETWORK_DISCONNECTED_ACTIVE:
        return "network_disconnected_active";
    case CONTROLLER_STATE_COUNT:
        break;
    }
    return "unknown";
}

/* Derives a single explicit state from the current wifi mode plus mybot and
 * network liveness.  Actual wifi_mode remains the source of truth; desired mode
 * is handled separately by reconcile_wifi_mode(). */
static void controller_sync_state(app_runtime_t *runtime) {
    controller_state_t next_state = CONTROLLER_STATE_WIFI_IDLE;

    if (runtime->wifi_mode == APP_WIFI_PROVISIONING) {
        next_state = CONTROLLER_STATE_PROVISIONING;
    } else if (runtime->wifi_mode != APP_WIFI_NETWORK) {
        next_state = CONTROLLER_STATE_WIFI_IDLE;
    } else if (runtime->mybot_active) {
        next_state = runtime->network_connected
                         ? CONTROLLER_STATE_NETWORK_CONNECTED_ACTIVE
                         : CONTROLLER_STATE_NETWORK_DISCONNECTED_ACTIVE;
    } else {
        next_state = runtime->network_connected
                         ? CONTROLLER_STATE_NETWORK_CONNECTED_STOPPED
                         : CONTROLLER_STATE_NETWORK_DISCONNECTED;
    }

    if (next_state != runtime->state) {
        MYBOT_LOGI(TAG, "state transition: %s -> %s",
                controller_state_name(runtime->state),
                controller_state_name(next_state));
    }
    runtime->state = next_state;
}

static controller_event_kind_t controller_event_from_mybot(mybot_event_type_t type) {
    switch (type) {
    case MYBOT_EVENT_BUTTON_VOLUME_UP:
        return CONTROLLER_EVENT_VOLUME_UP;
    case MYBOT_EVENT_BUTTON_VOLUME_DOWN:
        return CONTROLLER_EVENT_VOLUME_DOWN;
    case MYBOT_EVENT_BUTTON_CONVERSATION_TOGGLE:
        return CONTROLLER_EVENT_CONVERSATION_TOGGLE;
    case MYBOT_EVENT_BUTTON_PROVISIONING_REQUEST:
        return CONTROLLER_EVENT_PROVISIONING_REQUEST;
    case MYBOT_EVENT_NETWORK_CONNECTED:
        return CONTROLLER_EVENT_NETWORK_CONNECTED;
    case MYBOT_EVENT_NETWORK_DISCONNECTED:
        return CONTROLLER_EVENT_NETWORK_DISCONNECTED;
    case MYBOT_EVENT_NETWORK_FAILED:
        return CONTROLLER_EVENT_NETWORK_FAILED;
    case MYBOT_EVENT_PROVISIONING_COMPLETED:
        return CONTROLLER_EVENT_PROVISIONING_COMPLETED;
    case MYBOT_EVENT_PROVISIONING_FAILED:
        return CONTROLLER_EVENT_PROVISIONING_FAILED;
    case MYBOT_EVENT_TYPE_COUNT:
        break;
    }
    return CONTROLLER_EVENT_NONE;
}

static const char *controller_event_name(controller_event_kind_t event) {
    switch (event) {
    case CONTROLLER_EVENT_NONE:
        return "none";
    case CONTROLLER_EVENT_VOLUME_UP:
        return "volume_up";
    case CONTROLLER_EVENT_VOLUME_DOWN:
        return "volume_down";
    case CONTROLLER_EVENT_CONVERSATION_TOGGLE:
        return "conversation_toggle";
    case CONTROLLER_EVENT_PROVISIONING_REQUEST:
        return "provisioning_request";
    case CONTROLLER_EVENT_NETWORK_CONNECTED:
        return "network_connected";
    case CONTROLLER_EVENT_NETWORK_DISCONNECTED:
        return "network_disconnected";
    case CONTROLLER_EVENT_NETWORK_FAILED:
        return "network_failed";
    case CONTROLLER_EVENT_PROVISIONING_COMPLETED:
        return "provisioning_completed";
    case CONTROLLER_EVENT_PROVISIONING_FAILED:
        return "provisioning_failed";
    case CONTROLLER_EVENT_POLL_NETWORK_STATE:
        return "poll_network_state";
    case CONTROLLER_EVENT_POLL_PROVISIONING_STATE:
        return "poll_provisioning_state";
    case CONTROLLER_EVENT_POLL_WIFI_RECONCILE:
        return "poll_wifi_reconcile";
    case CONTROLLER_EVENT_POLL_MYBOT_RUNNING:
        return "poll_mybot_running";
    case CONTROLLER_EVENT_POLL_MYBOT_RESTART:
        return "poll_mybot_restart";
    case CONTROLLER_EVENT_COUNT:
        break;
    }
    return "unknown";
}

static uint32_t next_wifi_generation(app_runtime_t *runtime) {
    if (++runtime->wifi_generation == 0) {
        ++runtime->wifi_generation;
    }
    return runtime->wifi_generation;
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

static void stop_mybot(app_runtime_t *runtime) {
    if (runtime->mybot_active) {
        MYBOT_LOGI(TAG, "stopping mybot SDK");
        mybot_stop();
        runtime->mybot_active = false;
    }
    /* The shared playback module owns the audio power vote; do not release it. */
    runtime->audio_power_acquired = false;
    runtime->mybot_restart_at = 0;
}

static int start_mybot(app_runtime_t *runtime) {
    if (runtime->mybot_active || !mybot_network_is_connected()) {
        return 0;
    }
    mybot_prompt_player_bk725x_stop();
    mybot_audio_bk725x_shared_playback_start();
    if (bk725x_platform_adapters_register() < 0) {
        return -1;
    }
    /* The shared playback module holds the audio power vote
     * (acquired above or in start_provisioning). */
    runtime->audio_power_acquired = true;

    /* The SDK Wi-Fi adapter consumes this connected snapshot during mybot_start(). */
    (void)mybot_connectivity_publish(MYBOT_CONNECTIVITY_CONNECTED);
    MYBOT_LOGI(TAG, "starting mybot SDK");
    if (mybot_start(&runtime->config) < 0) {
        MYBOT_LOGE(TAG, "mybot SDK start failed");
        runtime->audio_power_acquired = false;
        return -1;
    }

    runtime->mybot_active = true;
    mybot_prompt_player_bk725x_play_success();
    runtime->mybot_restart_at = 0;
    MYBOT_LOGI(TAG, "mybot SDK started");
    return 0;
}

static int start_network(app_runtime_t *runtime) {
    uint32_t generation = next_wifi_generation(runtime);

    MYBOT_LOGI(TAG, "starting normal STA networking");
    (void)mybot_connectivity_publish(MYBOT_CONNECTIVITY_DISCONNECTED);
    if (mybot_network_start(generation) < 0) {
        MYBOT_LOGE(TAG, "normal STA networking start failed");
        if (mybot_network_stop() < 0) {
            MYBOT_LOGE(TAG, "failed normal STA startup cleanup is incomplete");
        }
        return -1;
    }
    runtime->wifi_mode = APP_WIFI_NETWORK;
    runtime->network_connected = false;
    runtime->wifi_retry_at = 0;
    show_display_screen(runtime, MYBOT_DISPLAY_SCREEN_WIFI_DISCONNECTED);
    return 0;
}

static int start_provisioning(app_runtime_t *runtime) {
    uint32_t generation = next_wifi_generation(runtime);

    MYBOT_LOGI(TAG, "starting APSTA provisioning");
    if (mybot_provisioning_start(runtime->config.device_id, generation) < 0) {
        MYBOT_LOGE(TAG, "APSTA provisioning start failed");
        if (mybot_provisioning_stop() < 0) {
            MYBOT_LOGE(TAG, "failed APSTA startup cleanup is incomplete");
        }
        return -1;
    }
    runtime->wifi_mode = APP_WIFI_PROVISIONING;
    runtime->network_connected = false;
    runtime->wifi_retry_at = 0;
    show_display_screen(runtime, MYBOT_DISPLAY_SCREEN_WIFI_PROVISIONING);

    /* Start the shared playback pipeline before the prompt player so the
     * prompt writes through the same audio path the SDK will use later. */
    if (mybot_audio_bk725x_shared_playback_start() < 0) {
        MYBOT_LOGW(TAG, "shared playback start failed, prompt may be silent");
    }

    if (!runtime->provisioning_prompt_announced) {
        runtime->provisioning_prompt_announced = true;
        if (mybot_prompt_player_bk725x_play_provisioning() < 0) {
            MYBOT_LOGW(TAG, "failed to start provisioning prompt");
        }
    }
    return 0;
}

static int stop_wifi_mode(app_runtime_t *runtime) {
    int result = 0;

    if (runtime->wifi_mode == APP_WIFI_NETWORK) {
        result = mybot_network_stop();
    } else if (runtime->wifi_mode == APP_WIFI_PROVISIONING) {
        result = mybot_provisioning_stop();
    }
    if (result < 0) {
        return -1;
    }

    runtime->wifi_mode = APP_WIFI_IDLE;
    runtime->network_connected = false;
    return 0;
}

static void reconcile_wifi_mode(app_runtime_t *runtime, uint32_t now) {
    if (runtime->wifi_mode == runtime->desired_wifi_mode && runtime->wifi_retry_at == 0) {
        return;
    }
    if (runtime->wifi_retry_at != 0 &&
        !controller_deadline_reached(now, runtime->wifi_retry_at)) {
        return;
    }

    if (runtime->wifi_mode != APP_WIFI_IDLE && stop_wifi_mode(runtime) < 0) {
        MYBOT_LOGE(TAG, "Wi-Fi mode stop incomplete; transition deferred");
        runtime->wifi_retry_at = now + MYBOT_RESTART_DELAY_MS;
        return;
    }

    int result = 0;
    if (runtime->desired_wifi_mode == APP_WIFI_NETWORK) {
        result = start_network(runtime);
    } else if (runtime->desired_wifi_mode == APP_WIFI_PROVISIONING) {
        result = start_provisioning(runtime);
    }
    if (result < 0) {
        runtime->wifi_retry_at = controller_now_ms() + MYBOT_RESTART_DELAY_MS;
    }
}

static void enter_provisioning(app_runtime_t *runtime) {
    if (runtime->desired_wifi_mode == APP_WIFI_PROVISIONING) {
        return;
    }

    MYBOT_LOGI(TAG, "provisioning requested");
    runtime->provisioning_prompt_announced = false;
    runtime->desired_wifi_mode = APP_WIFI_PROVISIONING;
    runtime->wifi_retry_at = 0;
    stop_mybot(runtime);
    (void)mybot_connectivity_publish(MYBOT_CONNECTIVITY_DISCONNECTED);
    reconcile_wifi_mode(runtime, controller_now_ms());
}

static void provisioning_completed(app_runtime_t *runtime) {
    if (runtime->wifi_mode != APP_WIFI_PROVISIONING) {
        return;
    }

    if (runtime->desired_wifi_mode != APP_WIFI_NETWORK) {
        MYBOT_LOGI(TAG, "APSTA provisioning completed");
        runtime->desired_wifi_mode = APP_WIFI_NETWORK;
        runtime->wifi_retry_at = 0;
    }
    mybot_prompt_player_bk725x_stop();
    mybot_audio_bk725x_shared_playback_start();
    reconcile_wifi_mode(runtime, controller_now_ms());
}

static void provisioning_failed(app_runtime_t *runtime) {
    uint32_t now;

    if (runtime->wifi_mode != APP_WIFI_PROVISIONING) {
        return;
    }

    now = controller_now_ms();
    if (runtime->wifi_retry_at != 0 &&
        !controller_deadline_reached(now, runtime->wifi_retry_at)) {
        return;
    }

    MYBOT_LOGE(TAG, "APSTA provisioning failed");
    runtime->desired_wifi_mode = APP_WIFI_PROVISIONING;
    if (stop_wifi_mode(runtime) < 0) {
        MYBOT_LOGE(TAG, "failed APSTA cleanup incomplete; retry deferred");
    }
    runtime->wifi_retry_at = now + MYBOT_RESTART_DELAY_MS;
}

static void update_network_state(app_runtime_t *runtime, bool connected) {
    if (runtime->wifi_mode != APP_WIFI_NETWORK || runtime->network_connected == connected) {
        return;
    }

    runtime->network_connected = connected;
    (void)mybot_connectivity_publish(connected ? MYBOT_CONNECTIVITY_CONNECTED
                                               : MYBOT_CONNECTIVITY_DISCONNECTED);
    if (connected) {
        MYBOT_LOGI(TAG, "normal STA connected");
        if (!runtime->mybot_active) {
            show_display_screen(runtime, MYBOT_DISPLAY_SCREEN_STARTING_SERVICES);
        }
        if (!runtime->mybot_active && start_mybot(runtime) < 0) {
            runtime->mybot_restart_at = controller_now_ms() + MYBOT_RESTART_DELAY_MS;
        }
    } else {
        MYBOT_LOGW(TAG, "normal STA disconnected");
        if (!runtime->mybot_active) {
            show_display_screen(runtime, MYBOT_DISPLAY_SCREEN_WIFI_DISCONNECTED);
        }
    }
}

typedef int (*controller_transition_fn)(app_runtime_t *runtime,
                                         const controller_event_t *event);
typedef bool (*controller_transition_guard_fn)(const app_runtime_t *runtime,
                                                const controller_event_t *event);

typedef struct {
    controller_event_kind_t event;
    uint32_t from_states;
    controller_transition_guard_fn guard;
    controller_transition_fn handler;
} controller_transition_t;

static bool event_is_from_current_wifi(const app_runtime_t *runtime,
                                       const controller_event_t *event) {
    if (event->source_generation == runtime->wifi_generation) {
        return true;
    }
    MYBOT_LOGW(TAG, "ignoring stale Wi-Fi event: type=%s generation=%u current=%u",
            controller_event_name(event->kind), (unsigned)event->source_generation,
            (unsigned)runtime->wifi_generation);
    return false;
}

static int transition_volume_up(app_runtime_t *runtime,
                                const controller_event_t *event) {
    (void)runtime;
    (void)event;
    (void)mybot_key_dispatcher_publish(MYBOT_KEY_ACTION_VOLUME_UP);
    return 0;
}

static int transition_volume_down(app_runtime_t *runtime,
                                  const controller_event_t *event) {
    (void)runtime;
    (void)event;
    (void)mybot_key_dispatcher_publish(MYBOT_KEY_ACTION_VOLUME_DOWN);
    return 0;
}

static bool guard_can_toggle_conversation(const app_runtime_t *runtime,
                                          const controller_event_t *event) {
    mybot_state_t state;

    (void)event;
    state = mybot_get_state();
    return runtime->mybot_active &&
           (state == MYBOT_STATE_READY || state == MYBOT_STATE_IN_CONVERSATION);
}

static int transition_conversation_toggle(app_runtime_t *runtime,
                                          const controller_event_t *event) {
    mybot_key_action_t action;

    (void)runtime;
    (void)event;

    switch (mybot_get_state()) {
    case MYBOT_STATE_READY:
        action = MYBOT_KEY_ACTION_CONVERSATION_START;
        break;
    case MYBOT_STATE_IN_CONVERSATION:
        action = MYBOT_KEY_ACTION_CONVERSATION_STOP;
        break;
    default:
        return 0;
    }
    (void)mybot_key_dispatcher_publish(action);
    return 0;
}

static int transition_provisioning_request(app_runtime_t *runtime,
                                           const controller_event_t *event) {
    (void)event;
    enter_provisioning(runtime);
    return 0;
}

static int transition_network_connected(app_runtime_t *runtime,
                                        const controller_event_t *event) {
    (void)event;
    update_network_state(runtime, true);
    return 0;
}

static int transition_network_disconnected(app_runtime_t *runtime,
                                           const controller_event_t *event) {
    (void)event;
    update_network_state(runtime, false);
    return 0;
}

static int transition_network_failed(app_runtime_t *runtime,
                                     const controller_event_t *event) {
    bool connected = mybot_network_is_connected();

    (void)event;
    update_network_state(runtime, connected);
    if (!connected && runtime->mybot_active) {
        MYBOT_LOGW(TAG, "normal STA connection attempt failed");
        (void)mybot_connectivity_publish(MYBOT_CONNECTIVITY_FAILED);
    }
    return 0;
}

static int transition_provisioning_completed(app_runtime_t *runtime,
                                             const controller_event_t *event) {
    (void)event;
    provisioning_completed(runtime);
    return 0;
}

static int transition_provisioning_failed(app_runtime_t *runtime,
                                          const controller_event_t *event) {
    (void)event;
    provisioning_failed(runtime);
    return 0;
}

static int transition_poll_network_state(app_runtime_t *runtime,
                                         const controller_event_t *event) {
    (void)event;
    update_network_state(runtime, mybot_network_is_connected());
    return 0;
}

static int transition_poll_provisioning_state(app_runtime_t *runtime,
                                              const controller_event_t *event) {
    mybot_provisioning_state_t state = mybot_provisioning_get_state();

    (void)event;
    if (state == MYBOT_PROVISIONING_STATE_COMPLETED) {
        provisioning_completed(runtime);
    } else if (state == MYBOT_PROVISIONING_STATE_FAILED) {
        provisioning_failed(runtime);
    }
    return 0;
}

static int transition_poll_wifi_reconcile(app_runtime_t *runtime,
                                          const controller_event_t *event) {
    reconcile_wifi_mode(runtime, event->now_ms);
    return 0;
}

static int transition_poll_mybot_running(app_runtime_t *runtime,
                                         const controller_event_t *event) {
    if (!mybot_is_running()) {
        MYBOT_LOGW(TAG, "mybot SDK stopped unexpectedly, state=%d", mybot_get_state());
        stop_mybot(runtime);
        runtime->mybot_restart_at = event->now_ms + MYBOT_RESTART_DELAY_MS;
    }
    return 0;
}

static int transition_poll_mybot_restart(app_runtime_t *runtime,
                                         const controller_event_t *event) {
    if (runtime->mybot_restart_at == 0 ||
        !controller_deadline_reached(event->now_ms, runtime->mybot_restart_at)) {
        return 0;
    }

    runtime->mybot_restart_at = 0;
    if (start_mybot(runtime) < 0) {
        runtime->mybot_restart_at = event->now_ms + MYBOT_RESTART_DELAY_MS;
    }
    return 0;
}

/* One transition per event type keeps verification local: the row declares the
 * valid source states and optional guard, while the handler performs only that
 * transition's side effects. */
static const controller_transition_t s_transitions[] = {
    {
        .event = CONTROLLER_EVENT_VOLUME_UP,
        .from_states = CONTROLLER_ACTIVE_STATES,
        .handler = transition_volume_up,
    },
    {
        .event = CONTROLLER_EVENT_VOLUME_DOWN,
        .from_states = CONTROLLER_ACTIVE_STATES,
        .handler = transition_volume_down,
    },
    {
        .event = CONTROLLER_EVENT_CONVERSATION_TOGGLE,
        .from_states = CONTROLLER_ACTIVE_STATES,
        .guard = guard_can_toggle_conversation,
        .handler = transition_conversation_toggle,
    },
    {
        .event = CONTROLLER_EVENT_PROVISIONING_REQUEST,
        .from_states = CONTROLLER_ALL_STATES,
        .handler = transition_provisioning_request,
    },
    {
        .event = CONTROLLER_EVENT_NETWORK_CONNECTED,
        .from_states = CONTROLLER_NETWORK_STATES,
        .guard = event_is_from_current_wifi,
        .handler = transition_network_connected,
    },
    {
        .event = CONTROLLER_EVENT_NETWORK_DISCONNECTED,
        .from_states = CONTROLLER_NETWORK_STATES,
        .guard = event_is_from_current_wifi,
        .handler = transition_network_disconnected,
    },
    {
        .event = CONTROLLER_EVENT_NETWORK_FAILED,
        .from_states = CONTROLLER_NETWORK_STATES,
        .guard = event_is_from_current_wifi,
        .handler = transition_network_failed,
    },
    {
        .event = CONTROLLER_EVENT_PROVISIONING_COMPLETED,
        .from_states = CONTROLLER_PROVISIONING_STATES,
        .guard = event_is_from_current_wifi,
        .handler = transition_provisioning_completed,
    },
    {
        .event = CONTROLLER_EVENT_PROVISIONING_FAILED,
        .from_states = CONTROLLER_PROVISIONING_STATES,
        .guard = event_is_from_current_wifi,
        .handler = transition_provisioning_failed,
    },
    {
        .event = CONTROLLER_EVENT_POLL_NETWORK_STATE,
        .from_states = CONTROLLER_NETWORK_STATES,
        .handler = transition_poll_network_state,
    },
    {
        .event = CONTROLLER_EVENT_POLL_PROVISIONING_STATE,
        .from_states = CONTROLLER_PROVISIONING_STATES,
        .handler = transition_poll_provisioning_state,
    },
    {
        .event = CONTROLLER_EVENT_POLL_WIFI_RECONCILE,
        .from_states = CONTROLLER_ALL_STATES,
        .handler = transition_poll_wifi_reconcile,
    },
    {
        .event = CONTROLLER_EVENT_POLL_MYBOT_RUNNING,
        .from_states = CONTROLLER_ACTIVE_STATES,
        .handler = transition_poll_mybot_running,
    },
    {
        .event = CONTROLLER_EVENT_POLL_MYBOT_RESTART,
        .from_states = CONTROLLER_NETWORK_STOPPED_STATES,
        .handler = transition_poll_mybot_restart,
    },
};

static void controller_dispatch(app_runtime_t *runtime,
                                const controller_event_t *event) {
    uint32_t state_flag;

    controller_sync_state(runtime);
    state_flag = controller_state_flag(runtime->state);

    for (size_t i = 0; i < sizeof(s_transitions) / sizeof(s_transitions[0]); ++i) {
        const controller_transition_t *transition = &s_transitions[i];
        if (transition->event != event->kind ||
            (transition->from_states & state_flag) == 0) {
            continue;
        }
        if (transition->guard && !transition->guard(runtime, event)) {
            return;
        }
        if (transition->handler) {
            (void)transition->handler(runtime, event);
        }
        controller_sync_state(runtime);
        return;
    }

    MYBOT_LOGD(TAG, "no transition matched event=%s state=%s",
            controller_event_name(event->kind),
            controller_state_name(runtime->state));
}

static void handle_event(app_runtime_t *runtime, const mybot_event_t *event) {
    controller_event_t controller_event = {
        .kind = controller_event_from_mybot(event->type),
        .now_ms = controller_now_ms(),
        .source_generation = event->source_generation,
    };

    if (controller_event.kind == CONTROLLER_EVENT_NONE) {
        MYBOT_LOGW(TAG, "discarding unknown controller event type=%d",
                (int)event->type);
        return;
    }

    controller_dispatch(runtime, &controller_event);
}

static void controller_tick(app_runtime_t *runtime) {
    uint32_t now = controller_now_ms();
    const controller_event_t events[] = {
        {
            .kind = CONTROLLER_EVENT_POLL_NETWORK_STATE,
            .now_ms = now,
        },
        {
            .kind = CONTROLLER_EVENT_POLL_PROVISIONING_STATE,
            .now_ms = now,
        },
        {
            .kind = CONTROLLER_EVENT_POLL_WIFI_RECONCILE,
            .now_ms = now,
        },
        {
            .kind = CONTROLLER_EVENT_POLL_MYBOT_RUNNING,
            .now_ms = now,
        },
        {
            .kind = CONTROLLER_EVENT_POLL_MYBOT_RESTART,
            .now_ms = now,
        },
    };

    for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); ++i) {
        controller_dispatch(runtime, &events[i]);
    }
}

static void controller_cleanup(app_runtime_t *runtime) {
    mybot_prompt_player_bk725x_stop();
    mybot_audio_bk725x_shared_playback_start();
    stop_mybot(runtime);
    mybot_audio_bk725x_shared_playback_stop();
    if (runtime->wifi_mode == APP_WIFI_NETWORK) {
        mybot_network_stop();
    } else if (runtime->wifi_mode == APP_WIFI_PROVISIONING) {
        mybot_provisioning_stop();
    }
    if (runtime->button_initialized) {
        mybot_button_deinit();
    }
    if (runtime->event_initialized) {
        mybot_event_deinit();
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
    bool configured = false;

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

    if (mybot_network_is_configured(&configured) < 0) {
        MYBOT_LOGE(TAG, "failed to inspect saved Wi-Fi credentials");
        goto cleanup;
    }
    MYBOT_LOGI(TAG, "Wi-Fi provisioning state: %s", configured ? "configured" : "new device");

    if (configured) {
        runtime.desired_wifi_mode = APP_WIFI_NETWORK;
    } else {
        runtime.desired_wifi_mode = APP_WIFI_PROVISIONING;
    }
    reconcile_wifi_mode(&runtime, controller_now_ms());

    MYBOT_LOGI(TAG, "application controller ready");
    for (;;) {
        mybot_event_t event;
        if (mybot_event_wait(&event, CONTROLLER_WAIT_MS) == 0) {
            handle_event(&runtime, &event);
        }
        controller_tick(&runtime);
    }

cleanup:
    MYBOT_LOGE(TAG, "application controller stopped during initialization");
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
