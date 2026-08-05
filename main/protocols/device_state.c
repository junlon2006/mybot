#include "device_state.h"
#include "device_api.h"
#include "flash/flash_device.h"

#include <api/aosl_log.h>
#include <api/aosl_atomic.h>

#include <string.h>
#include <stdio.h>


#define MYBOT_DEVICE_AUTH_FLASH_KEY "device_auth"
#define MYBOT_DEVICE_AUTH_VERSION   1U

typedef struct {
    uint32_t version;
    char server_base[MYBOT_DEVICE_API_MAX_URL];
    char device_id[MYBOT_DEVICE_API_MAX_ID];
    char device_token[MYBOT_DEVICE_API_MAX_TOKEN];
} mybot_device_auth_record_t;
/* ----------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------- */
static struct {
    char server_base[MYBOT_DEVICE_API_MAX_URL];
    char device_id[MYBOT_DEVICE_API_MAX_ID];
    char firmware_ver[64];
    char hw_model[64];
    mybot_device_state_callbacks_t cbs;

    aosl_atomic_t state;   /* atomic: also read by the main/SDK threads */

    /* Pairing phase */
    char pair_token[MYBOT_DEVICE_API_MAX_TOKEN];
    int  pair_poll_interval;    /* seconds between polls */
    int  pair_tick_counter;     /* counts tick() calls (100ms each) */

    /* Runtime phase */
    char device_token[MYBOT_DEVICE_API_MAX_TOKEN];
    int  runtime_poll_interval;
    int  runtime_tick_counter;

    /* Requests are atomically published by the main/SDK threads and consumed
     * by the state_mpq thread. */
    char conversation_id[MYBOT_DEVICE_API_MAX_ID];
    aosl_atomic_t conversation_requested;    /* user wants to start */
    aosl_atomic_t stop_request;              /* mybot_stop_request_t */

    /* One-shot action flag consumed by tick() */
    aosl_atomic_t start_pairing_flag;
} s_state;

typedef enum {
    MYBOT_STOP_REQUEST_NONE = 0,
    MYBOT_STOP_REQUEST_DEVICE_HANGUP,
    MYBOT_STOP_REQUEST_ERROR,
} mybot_stop_request_t;

/* ----------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------- */

static const char *s_name[] = {
    "unprovisioned", "pairing", "awaiting_claim", "runtime", "in_conversation"
};

const char *mybot_device_state_name(mybot_device_state_t s)
{
    if ((size_t)s >= sizeof(s_name) / sizeof(s_name[0])) {
        return "?";
    }
    return s_name[s];
}

static mybot_device_state_t current_state(void)
{
    return (mybot_device_state_t)aosl_atomic_read(&s_state.state);
}

static void set_state(mybot_device_state_t new_state)
{
    if (current_state() == new_state) {
        return;
    }
    aosl_atomic_set(&s_state.state, (intptr_t)new_state);
    AOSL_LOG_INF("%s", s_name[new_state]);
    if (s_state.cbs.on_state_changed) {
        s_state.cbs.on_state_changed(new_state);
    }
}

static bool api_rejected_device_auth(int ret)
{
    return ret == 401 || ret == 403 || ret == 409;
}

static int persist_device_auth(void)
{
    mybot_device_auth_record_t record;
    memset(&record, 0, sizeof(record));
    record.version = MYBOT_DEVICE_AUTH_VERSION;
    strncpy(record.server_base, s_state.server_base,
            sizeof(record.server_base) - 1);
    strncpy(record.device_id, s_state.device_id,
            sizeof(record.device_id) - 1);
    strncpy(record.device_token, s_state.device_token,
            sizeof(record.device_token) - 1);
    return mybot_flash_write(MYBOT_DEVICE_AUTH_FLASH_KEY, &record,
                             sizeof(record));
}

static bool load_device_auth(void)
{
    mybot_device_auth_record_t record;
    size_t len = 0;
    memset(&record, 0, sizeof(record));

    int ret = mybot_flash_read(MYBOT_DEVICE_AUTH_FLASH_KEY, &record,
                               sizeof(record), &len);
    if (ret == MYBOT_FLASH_NOT_FOUND) {
        return false;
    }
    if (ret < 0 || len != sizeof(record) ||
        record.version != MYBOT_DEVICE_AUTH_VERSION ||
        record.server_base[sizeof(record.server_base) - 1] != '\0' ||
        record.device_id[sizeof(record.device_id) - 1] != '\0' ||
        record.device_token[sizeof(record.device_token) - 1] != '\0' ||
        strcmp(record.server_base, s_state.server_base) != 0 ||
        strcmp(record.device_id, s_state.device_id) != 0 ||
        record.device_token[0] == '\0') {
        if (ret < 0) {
            AOSL_LOG_ERR("failed to read persisted device credential");
        }
        (void)mybot_flash_erase(MYBOT_DEVICE_AUTH_FLASH_KEY);
        return false;
    }

    strncpy(s_state.device_token, record.device_token,
            sizeof(s_state.device_token) - 1);
    return true;
}

static void clear_device_auth(void)
{
    s_state.device_token[0] = '\0';
    if (mybot_flash_erase(MYBOT_DEVICE_AUTH_FLASH_KEY) < 0) {
        AOSL_LOG_ERR("failed to erase persisted device credential");
    }
}

static void restart_pairing_after_auth_rejection(void)
{
    clear_device_auth();
    set_state(MYBOT_DEVICE_STATE_UNPROVISIONED);
    aosl_atomic_set(&s_state.start_pairing_flag, true);
}

const char *mybot_device_state_get_token(void)
{
    return (current_state() == MYBOT_DEVICE_STATE_RUNTIME ||
            current_state() == MYBOT_DEVICE_STATE_IN_CONVERSATION)
           ? s_state.device_token : NULL;
}

mybot_device_state_t mybot_device_state_get(void)
{
    return current_state();
}

/* ----------------------------------------------------------
 * Action: POST /devices/pair-codes
 * ---------------------------------------------------------- */
static void action_create_pair_code(void)
{
    mybot_device_pair_code_t resp;
    memset(&resp, 0, sizeof(resp));

    int ret = mybot_device_api_create_pair_code(
        s_state.server_base, s_state.device_id,
        s_state.firmware_ver, s_state.hw_model, &resp);
    if (ret != 0) {
        AOSL_LOG_ERR("pair-code request failed");
        set_state(MYBOT_DEVICE_STATE_UNPROVISIONED);
        return;
    }

    AOSL_LOG_INF("pair-code obtained: code=%s, poll=%ds",
                 resp.code, resp.poll_after_seconds);

    /* Save pair token and poll settings */
    strncpy(s_state.pair_token, resp.pair_token, sizeof(s_state.pair_token) - 1);
    /* Enforce a minimum 3 s poll interval even if the server omits or
     * undershoots poll_after_seconds (otherwise the device would busy-poll). */
    s_state.pair_poll_interval = resp.poll_after_seconds >= 3
                                     ? resp.poll_after_seconds : 3;
    s_state.pair_tick_counter  = 0;

    /* Clear any old device token */
    s_state.device_token[0] = '\0';

    /* Notify app to broadcast pair code */
    if (s_state.cbs.on_pair_code) {
        s_state.cbs.on_pair_code(resp.code);
    }

    set_state(MYBOT_DEVICE_STATE_AWAITING_CLAIM);
}

/* ----------------------------------------------------------
 * Action: poll binding-status (pairing phase)
 * ---------------------------------------------------------- */
static void action_poll_binding_pair(void)
{
    char auth[MYBOT_DEVICE_API_MAX_TOKEN + 16];
    snprintf(auth, sizeof(auth), "Pair %s", s_state.pair_token);

    mybot_device_binding_t resp;
    memset(&resp, 0, sizeof(resp));

    int ret = mybot_device_api_get_binding_status(
        s_state.server_base, s_state.device_id, auth, &resp);
    if (api_rejected_device_auth(ret)) {
        AOSL_LOG_WRN("pair credential rejected (HTTP %d), requesting a new pair code", ret);
        aosl_atomic_set(&s_state.start_pairing_flag, true);
        return;
    }
    if (ret != 0) {
        AOSL_LOG_ERR("bind poll (pair) failed, retrying");
        return;
    }

    AOSL_LOG_INF("bind poll -> status=%s", resp.status);

    if (strcmp(resp.status, "pending") == 0) {
        s_state.pair_poll_interval = resp.poll_after_seconds >= 3
                                         ? resp.poll_after_seconds : 3;
        /* stay in awaiting_claim */
    } else if (strcmp(resp.status, "bound") == 0) {
        if (resp.device_token[0]) {
            strncpy(s_state.device_token, resp.device_token,
                    sizeof(s_state.device_token) - 1);
        }
        if (!s_state.device_token[0]) {
            AOSL_LOG_ERR("bound response did not include the one-time device credential");
            return;
        }
        if (persist_device_auth() < 0) {
            AOSL_LOG_ERR("failed to persist device credential, retrying");
            return;
        }
        AOSL_LOG_INF("device credential persisted");
        set_state(MYBOT_DEVICE_STATE_RUNTIME);
        s_state.runtime_poll_interval = resp.poll_after_seconds > 0
                                            ? resp.poll_after_seconds : 30;
        s_state.runtime_tick_counter  = 0;
    } else if (strcmp(resp.status, "expired") == 0) {
        AOSL_LOG_INF("pair code expired, re-pairing");
        /* The top-level pairing handler in tick() re-runs the pair-code
         * request on the next tick. */
        aosl_atomic_set(&s_state.start_pairing_flag, true);
    } else if (strcmp(resp.status, "unbound") == 0) {
        /* Shouldn't happen during pairing, but handle gracefully */
        AOSL_LOG_INF("unexpected unbound during pairing");
        set_state(MYBOT_DEVICE_STATE_UNPROVISIONED);
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
    char auth[MYBOT_DEVICE_API_MAX_TOKEN + 16];
    snprintf(auth, sizeof(auth), "Device %s", s_state.device_token);

    mybot_device_binding_t resp;
    memset(&resp, 0, sizeof(resp));

    int ret = mybot_device_api_get_binding_status(
        s_state.server_base, s_state.device_id, auth, &resp);
    if (api_rejected_device_auth(ret)) {
        AOSL_LOG_WRN("device credential rejected (HTTP %d), re-pairing", ret);
        restart_pairing_after_auth_rejection();
        return;
    }
    if (ret != 0) {
        AOSL_LOG_ERR("bind poll (device) failed, retrying");
        return;
    }

    if (strcmp(resp.status, "bound") == 0) {
        s_state.runtime_poll_interval = resp.poll_after_seconds > 0
                                            ? resp.poll_after_seconds : 30;
    } else if (strcmp(resp.status, "unbound") == 0) {
        AOSL_LOG_INF("device unbound by user");
        restart_pairing_after_auth_rejection();
    } else {
        AOSL_LOG_ERR("unexpected runtime status: %s", resp.status);
    }
}

/* ----------------------------------------------------------
 * Action: start conversation
 * ---------------------------------------------------------- */
static void action_start_conversation(void)
{
    mybot_device_conversation_t resp;
    memset(&resp, 0, sizeof(resp));

    int ret = mybot_device_api_start_conversation(
        s_state.server_base, s_state.device_id,
        s_state.device_token, NULL, &resp);
    if (api_rejected_device_auth(ret)) {
        AOSL_LOG_WRN("device credential rejected while starting conversation (HTTP %d)", ret);
        restart_pairing_after_auth_rejection();
        return;
    }
    if (ret != 0) {
        AOSL_LOG_ERR("start conversation failed");
        return;
    }

    strncpy(s_state.conversation_id, resp.conversation_id,
            sizeof(s_state.conversation_id) - 1);

    AOSL_LOG_INF("conversation started: %s, channel=%s, uid=%s",
                 s_state.conversation_id, resp.rtc_channel, resp.rtc_uid);

    set_state(MYBOT_DEVICE_STATE_IN_CONVERSATION);

    /* Notify app */
    if (s_state.cbs.on_conversation_start) {
        mybot_conversation_params_t params;
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
    if (!s_state.conversation_id[0]) {
        return;
    }

    int ret = mybot_device_api_stop_conversation(
        s_state.server_base, s_state.device_id,
        s_state.device_token, s_state.conversation_id, reason);

    AOSL_LOG_INF("conversation stopped");
    s_state.conversation_id[0] = '\0';

    if (s_state.cbs.on_conversation_stop) {
        s_state.cbs.on_conversation_stop();
    }

    if (api_rejected_device_auth(ret)) {
        AOSL_LOG_WRN("device credential rejected while stopping conversation (HTTP %d)", ret);
        restart_pairing_after_auth_rejection();
    } else {
        set_state(MYBOT_DEVICE_STATE_RUNTIME);
    }
}

/* ----------------------------------------------------------
 * Public API
 * ---------------------------------------------------------- */

int mybot_device_state_init(const char *server_base, const char *device_id,
                      const char *firmware_ver, const char *hw_model,
                      mybot_device_state_callbacks_t *cbs)
{
    if (!server_base || !device_id) {
        return -1;
    }

    memset(&s_state, 0, sizeof(s_state));

    strncpy(s_state.server_base, server_base, sizeof(s_state.server_base) - 1);
    strncpy(s_state.device_id, device_id, sizeof(s_state.device_id) - 1);
    if (firmware_ver) {
        strncpy(s_state.firmware_ver, firmware_ver, sizeof(s_state.firmware_ver) - 1);
    }
    if (hw_model) {
        strncpy(s_state.hw_model, hw_model, sizeof(s_state.hw_model) - 1);
    }
    if (cbs) {
        s_state.cbs = *cbs;
    }

    if (load_device_auth()) {
        s_state.runtime_poll_interval = 30;
        set_state(MYBOT_DEVICE_STATE_RUNTIME);
        AOSL_LOG_INF("restored persisted device credential");
    } else {
        set_state(MYBOT_DEVICE_STATE_UNPROVISIONED);
        aosl_atomic_set(&s_state.start_pairing_flag, true);
    }

    return 0;
}

void mybot_device_state_tick(void)
{
    /* A pending pairing request (first boot, expired pair code, or the user
     * pressing 'p') starts a fresh pair-code request from ANY state. If a
     * conversation is active, end it first so the RTC connection is torn down
     * before the device is rebound. */
    if (aosl_atomic_xchg(&s_state.start_pairing_flag, false)) {
        if (current_state() == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
            action_stop_conversation("re-pair");
        }
        clear_device_auth();
        s_state.conversation_id[0] = '\0';
        set_state(MYBOT_DEVICE_STATE_PAIRING);
        action_create_pair_code();
        return;
    }

    if (current_state() == MYBOT_DEVICE_STATE_UNPROVISIONED) {
        /* Unprovisioned with no pending pairing request — wait for 'p'. */
        return;
    }

    if (current_state() == MYBOT_DEVICE_STATE_PAIRING) {
        /* This state is transient — action_create_pair_code() moves out */
        return;
    }

    if (current_state() == MYBOT_DEVICE_STATE_AWAITING_CLAIM) {
        s_state.pair_tick_counter++;
        /* 100ms per tick, convert poll interval to ticks */
        int interval_ticks = s_state.pair_poll_interval * 10;
        if (s_state.pair_tick_counter >= interval_ticks) {
            s_state.pair_tick_counter = 0;
            action_poll_binding_pair();
        }
        return;
    }

    if (current_state() == MYBOT_DEVICE_STATE_RUNTIME) {
        /* Check for user requests */
        if (aosl_atomic_xchg(&s_state.conversation_requested, false)) {
            aosl_atomic_set(&s_state.stop_request, MYBOT_STOP_REQUEST_NONE);
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

    if (current_state() == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        mybot_stop_request_t request =
            (mybot_stop_request_t)aosl_atomic_xchg(
                &s_state.stop_request, MYBOT_STOP_REQUEST_NONE);
        if (request != MYBOT_STOP_REQUEST_NONE) {
            const char *reason =
                request == MYBOT_STOP_REQUEST_DEVICE_HANGUP
                    ? "device_hangup" : "error";
            action_stop_conversation(reason);
        }
        return;
    }
}

void mybot_device_state_request_pair(void)
{
    aosl_atomic_set(&s_state.start_pairing_flag, true);
}

void mybot_device_state_request_start(void)
{
    if (current_state() != MYBOT_DEVICE_STATE_RUNTIME) {
        AOSL_LOG_ERR("cannot start: not in runtime");
        return;
    }
    aosl_atomic_set(&s_state.conversation_requested, true);
}

void mybot_device_state_request_stop(void)
{
    if (current_state() != MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        AOSL_LOG_ERR("cannot stop: not in conversation");
        return;
    }
    aosl_atomic_set(&s_state.stop_request, MYBOT_STOP_REQUEST_DEVICE_HANGUP);
}

void mybot_device_state_notify_conversation_ended(void)
{
    /* Called from an RTC SDK callback thread on connection loss/error. Only
     * flag the stop here — the actual teardown (HTTP stop + RTC leave) runs
     * on the state_mpq thread via mybot_device_state_tick(), avoiding
     * re-entrant SDK calls from inside an SDK callback. */
    if (current_state() == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        aosl_atomic_set(&s_state.stop_request, MYBOT_STOP_REQUEST_ERROR);
    }
}
