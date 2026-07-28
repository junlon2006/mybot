#include "device_state.h"
#include "device_api.h"

#include <api/aosl_log.h>

#include <string.h>
#include <stdio.h>

/* ----------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------- */
static struct {
    char server_base[DEVICE_API_MAX_URL];
    char device_id[DEVICE_API_MAX_ID];
    char firmware_ver[64];
    char hw_model[64];
    device_state_callbacks_t cbs;

    device_state_t state;

    /* Pairing phase */
    char pair_token[DEVICE_API_MAX_TOKEN];
    int  pair_poll_interval;    /* seconds between polls */
    int  pair_tick_counter;     /* counts tick() calls (100ms each) */

    /* Runtime phase */
    char device_token[DEVICE_API_MAX_TOKEN];
    int  runtime_poll_interval;
    int  runtime_tick_counter;

    /* Conversation */
    char conversation_id[DEVICE_API_MAX_ID];
    bool conversation_requested;    /* user wants to start */
    bool stop_requested;            /* user wants to stop */
    bool pairing_requested;         /* user wants to pair */

    /* One-shot action flags consumed by tick() */
    bool start_pairing_flag;
} s_state;

/* ----------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------- */

static const char *s_name[] = {
    "unprovisioned", "pairing", "awaiting_claim", "runtime", "in_conversation"
};

const char *device_state_name(device_state_t s)
{
    if ((size_t)s >= sizeof(s_name) / sizeof(s_name[0]))
        return "?";
    return s_name[s];
}

static void set_state(device_state_t new_state)
{
    if (s_state.state == new_state)
        return;
    s_state.state = new_state;
    AOSL_LOG_INF("%s", s_name[new_state]);
    if (s_state.cbs.on_state_changed)
        s_state.cbs.on_state_changed(new_state);
}

const char *device_state_get_token(void)
{
    return (s_state.state == DEVICE_STATE_RUNTIME ||
            s_state.state == DEVICE_STATE_IN_CONVERSATION)
           ? s_state.device_token : NULL;
}

device_state_t device_state_get(void)
{
    return s_state.state;
}

/* ----------------------------------------------------------
 * Action: POST /devices/pair-codes
 * ---------------------------------------------------------- */
static void action_create_pair_code(void)
{
    device_pair_code_t resp;
    memset(&resp, 0, sizeof(resp));

    int ret = device_api_create_pair_code(
        s_state.server_base, s_state.device_id,
        s_state.firmware_ver, s_state.hw_model, &resp);
    if (ret < 0) {
        AOSL_LOG_ERR("pair-code request failed");
        set_state(DEVICE_STATE_UNPROVISIONED);
        return;
    }

    AOSL_LOG_INF("pair-code obtained: code=%s, poll=%ds",
            resp.code, resp.poll_after_seconds);

    /* Save pair token and poll settings */
    strncpy(s_state.pair_token, resp.pair_token, sizeof(s_state.pair_token) - 1);
    s_state.pair_poll_interval = resp.poll_after_seconds;
    s_state.pair_tick_counter  = 0;

    /* Clear any old device token */
    s_state.device_token[0] = '\0';

    /* Notify app to broadcast pair code */
    if (s_state.cbs.on_pair_code)
        s_state.cbs.on_pair_code(resp.code);

    set_state(DEVICE_STATE_AWAITING_CLAIM);
}

/* ----------------------------------------------------------
 * Action: poll binding-status (pairing phase)
 * ---------------------------------------------------------- */
static void action_poll_binding_pair(void)
{
    char auth[DEVICE_API_MAX_TOKEN + 16];
    snprintf(auth, sizeof(auth), "Pair %s", s_state.pair_token);

    device_binding_t resp;
    memset(&resp, 0, sizeof(resp));

    int ret = device_api_get_binding_status(
        s_state.server_base, s_state.device_id, auth, &resp);
    if (ret < 0) {
        AOSL_LOG_ERR("bind poll (pair) failed, retrying");
        return;
    }

    AOSL_LOG_INF("bind poll -> status=%s", resp.status);

    if (strcmp(resp.status, "pending") == 0) {
        s_state.pair_poll_interval = resp.poll_after_seconds > 0
                                         ? resp.poll_after_seconds : 3;
        /* stay in awaiting_claim */
    } else if (strcmp(resp.status, "bound") == 0) {
        /* Check if device_token was issued */
        if (resp.device_token[0]) {
            strncpy(s_state.device_token, resp.device_token,
                    sizeof(s_state.device_token) - 1);
            AOSL_LOG_INF("device_token obtained");
        }
        set_state(DEVICE_STATE_RUNTIME);
        s_state.runtime_poll_interval = resp.poll_after_seconds > 0
                                            ? resp.poll_after_seconds : 30;
        s_state.runtime_tick_counter  = 0;
    } else if (strcmp(resp.status, "expired") == 0) {
        AOSL_LOG_INF("pair code expired, re-pairing");
        set_state(DEVICE_STATE_UNPROVISIONED);
        /* auto-trigger re-pair */
        s_state.start_pairing_flag = true;
    } else if (strcmp(resp.status, "unbound") == 0) {
        /* Shouldn't happen during pairing, but handle gracefully */
        AOSL_LOG_INF("unexpected unbound during pairing");
        set_state(DEVICE_STATE_UNPROVISIONED);
    } else {
        /* unknown status */
        AOSL_LOG_ERR("unknown bind status: %s", resp.status);
    }
}

/* ----------------------------------------------------------
 * Action: poll binding-status (runtime phase)
 * ---------------------------------------------------------- */
static void action_poll_binding_runtime(void)
{
    char auth[DEVICE_API_MAX_TOKEN + 16];
    snprintf(auth, sizeof(auth), "Device %s", s_state.device_token);

    device_binding_t resp;
    memset(&resp, 0, sizeof(resp));

    int ret = device_api_get_binding_status(
        s_state.server_base, s_state.device_id, auth, &resp);
    if (ret < 0) {
        AOSL_LOG_ERR("bind poll (device) failed, retrying");
        return;
    }

    if (strcmp(resp.status, "bound") == 0) {
        s_state.runtime_poll_interval = resp.poll_after_seconds > 0
                                            ? resp.poll_after_seconds : 30;
    } else if (strcmp(resp.status, "unbound") == 0) {
        AOSL_LOG_INF("device unbound by user");
        s_state.device_token[0] = '\0';
        set_state(DEVICE_STATE_UNPROVISIONED);
    } else {
        AOSL_LOG_ERR("unexpected runtime status: %s", resp.status);
    }
}

/* ----------------------------------------------------------
 * Action: start conversation
 * ---------------------------------------------------------- */
static void action_start_conversation(void)
{
    /* Start audio capture/playback BEFORE calling the API */
    if (s_state.cbs.on_audio_start)
        s_state.cbs.on_audio_start();

    device_conversation_t resp;
    memset(&resp, 0, sizeof(resp));

    int ret = device_api_start_conversation(
        s_state.server_base, s_state.device_id,
        s_state.device_token, NULL, &resp);
    if (ret < 0) {
        AOSL_LOG_ERR("start conversation failed");
        return;
    }

    strncpy(s_state.conversation_id, resp.conversation_id,
            sizeof(s_state.conversation_id) - 1);

    AOSL_LOG_INF("conversation started: %s, channel=%s, uid=%s",
            s_state.conversation_id, resp.rtc_channel, resp.rtc_uid);

    set_state(DEVICE_STATE_IN_CONVERSATION);

    /* Notify app */
    if (s_state.cbs.on_conversation_start) {
        conversation_params_t params;
        memset(&params, 0, sizeof(params));
        strncpy(params.conversation_id, resp.conversation_id, sizeof(params.conversation_id) - 1);
        strncpy(params.rtc_app_id, resp.rtc_app_id, sizeof(params.rtc_app_id) - 1);
        strncpy(params.rtc_channel, resp.rtc_channel, sizeof(params.rtc_channel) - 1);
        strncpy(params.rtc_uid, resp.rtc_uid, sizeof(params.rtc_uid) - 1);
        strncpy(params.rtc_token, resp.rtc_token, sizeof(params.rtc_token) - 1);
        s_state.cbs.on_conversation_start(&params);
    }
}

/* ----------------------------------------------------------
 * Action: stop conversation
 * ---------------------------------------------------------- */
static void action_stop_conversation(const char *reason)
{
    if (!s_state.conversation_id[0])
        return;

    device_api_stop_conversation(
        s_state.server_base, s_state.device_id,
        s_state.device_token, s_state.conversation_id, reason);

    AOSL_LOG_INF("conversation stopped");
    s_state.conversation_id[0] = '\0';

    set_state(DEVICE_STATE_RUNTIME);

    if (s_state.cbs.on_conversation_stop)
        s_state.cbs.on_conversation_stop();

    /* Stop audio capture/playback AFTER all RTC cleanup */
    if (s_state.cbs.on_audio_stop)
        s_state.cbs.on_audio_stop();
}

/* ----------------------------------------------------------
 * Public API
 * ---------------------------------------------------------- */

int device_state_init(const char *server_base, const char *device_id,
                      const char *firmware_ver, const char *hw_model,
                      device_state_callbacks_t *cbs)
{
    if (!server_base || !device_id)
        return -1;

    memset(&s_state, 0, sizeof(s_state));

    strncpy(s_state.server_base, server_base, sizeof(s_state.server_base) - 1);
    strncpy(s_state.device_id, device_id, sizeof(s_state.device_id) - 1);
    if (firmware_ver)
        strncpy(s_state.firmware_ver, firmware_ver, sizeof(s_state.firmware_ver) - 1);
    if (hw_model)
        strncpy(s_state.hw_model, hw_model, sizeof(s_state.hw_model) - 1);
    if (cbs)
        s_state.cbs = *cbs;

    /* Start in pairing mode */
    set_state(DEVICE_STATE_UNPROVISIONED);
    s_state.start_pairing_flag = true;

    return 0;
}

void device_state_tick(void)
{
    if (s_state.state == DEVICE_STATE_UNPROVISIONED) {
        if (s_state.start_pairing_flag) {
            s_state.start_pairing_flag = false;
            set_state(DEVICE_STATE_PAIRING);
            action_create_pair_code();
        }
        return;
    }

    if (s_state.state == DEVICE_STATE_PAIRING) {
        /* This state is transient — action_create_pair_code() moves out */
        return;
    }

    if (s_state.state == DEVICE_STATE_AWAITING_CLAIM) {
        s_state.pair_tick_counter++;
        /* 100ms per tick, convert poll interval to ticks */
        int interval_ticks = s_state.pair_poll_interval * 10;
        if (s_state.pair_tick_counter >= interval_ticks) {
            s_state.pair_tick_counter = 0;
            action_poll_binding_pair();
        }
        return;
    }

    if (s_state.state == DEVICE_STATE_RUNTIME) {
        /* Check for user requests */
        if (s_state.conversation_requested) {
            s_state.conversation_requested = false;
            s_state.stop_requested = false;
            action_start_conversation();
            return;
        }

        /* Periodic binding status poll */
        s_state.runtime_tick_counter++;
        int interval_ticks = s_state.runtime_poll_interval * 10;
        if (s_state.runtime_tick_counter >= interval_ticks) {
            s_state.runtime_tick_counter = 0;
            action_poll_binding_runtime();
        }
        return;
    }

    if (s_state.state == DEVICE_STATE_IN_CONVERSATION) {
        if (s_state.stop_requested) {
            s_state.stop_requested = false;
            action_stop_conversation("device_hangup");
        }
        return;
    }
}

void device_state_request_pair(void)
{
    s_state.start_pairing_flag = true;
}

void device_state_request_start(void)
{
    if (s_state.state != DEVICE_STATE_RUNTIME) {
        AOSL_LOG_ERR("cannot start: not in runtime");
        return;
    }
    s_state.conversation_requested = true;
}

void device_state_request_stop(void)
{
    if (s_state.state != DEVICE_STATE_IN_CONVERSATION) {
        AOSL_LOG_ERR("cannot stop: not in conversation");
        return;
    }
    s_state.stop_requested = true;
}

void device_state_notify_conversation_ended(void)
{
    if (s_state.state == DEVICE_STATE_IN_CONVERSATION) {
        action_stop_conversation("error");
    }
}
