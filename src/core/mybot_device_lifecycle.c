/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_device_lifecycle.h"
#include "mybot_device_client.h"
#include <mybot/platform/mybot_kv_store.h>

#include "mybot_kv_store_internal.h"

#include <api/aosl_log.h>
#include <api/aosl_atomic.h>

#include <string.h>
#include <stdio.h>

#define MYBOT_DEVICE_AUTH_KEY "device_auth"
#define MYBOT_DEVICE_AUTH_VERSION 1U
#define MYBOT_PAIR_RETRY_INITIAL_TICKS 30
#define MYBOT_PAIR_RETRY_MAX_TICKS 600
#define MYBOT_RTC_TOKEN_RETRY_INITIAL_TICKS 10
#define MYBOT_RTC_TOKEN_RETRY_MAX_TICKS 100
#define MYBOT_POLL_INTERVAL_MIN_SECONDS 3
#define MYBOT_POLL_INTERVAL_MAX_SECONDS 60

typedef struct {
    uint32_t version;
    char server_base[MYBOT_DEVICE_CLIENT_MAX_URL];
    char device_id[MYBOT_DEVICE_CLIENT_MAX_ID];
    char device_token[MYBOT_DEVICE_CLIENT_MAX_TOKEN];
} mybot_device_auth_record_t;
/* ----------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------- */
static struct {
    char server_base[MYBOT_DEVICE_CLIENT_MAX_URL];
    char device_id[MYBOT_DEVICE_CLIENT_MAX_ID];
    char firmware_ver[64];
    char hw_model[64];
    mybot_device_lifecycle_callbacks_t cbs;

    aosl_atomic_t state; /* atomic: also read by the main/SDK threads */

    /* Pairing phase */
    char pair_token[MYBOT_DEVICE_CLIENT_MAX_TOKEN];
    int pair_poll_interval; /* seconds between polls */
    int pair_tick_counter;  /* counts tick() calls (100 ms each) */
    int pair_retry_delay_ticks;
    int pair_retry_ticks_remaining;

    /* Runtime phase */
    char device_token[MYBOT_DEVICE_CLIENT_MAX_TOKEN];
    int runtime_poll_interval;
    int runtime_tick_counter;

    /* Requests are atomically published by application/SDK threads and
     * consumed by the state_mpq thread. */
    char conversation_id[MYBOT_DEVICE_CLIENT_MAX_ID];
    char rtc_channel[128];
    char rtc_uid[64];
    aosl_atomic_t conversation_requested; /* user wants to start */
    aosl_atomic_t stop_request;           /* mybot_stop_request_t */
    aosl_atomic_t rtc_token_renewal_requested;
    bool rtc_token_renewal_pending;
    int rtc_token_retry_delay_ticks;
    int rtc_token_retry_ticks_remaining;

    /* One-shot action flag consumed by tick() */
    aosl_atomic_t start_pairing_flag;
    aosl_atomic_t shutting_down;
    aosl_atomic_t network_available;
    aosl_atomic_t network_loss_pending;
    aosl_atomic_t network_generation;
} s_state;

typedef enum {
    MYBOT_STOP_REQUEST_NONE = 0,
    MYBOT_STOP_REQUEST_DEVICE_HANGUP,
    MYBOT_STOP_REQUEST_ERROR,
} mybot_stop_request_t;

/* ----------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------- */

static const char *s_name[] = {"unprovisioned", "pairing", "awaiting_claim", "runtime",
                               "in_conversation"};

const char *mybot_device_lifecycle_state_name(mybot_device_state_t s) {
    if ((size_t)s >= sizeof(s_name) / sizeof(s_name[0])) {
        return "?";
    }
    return s_name[s];
}

static mybot_device_state_t current_state(void) {
    return (mybot_device_state_t)aosl_atomic_read(&s_state.state);
}

static bool network_request_is_current(intptr_t generation) {
    return aosl_atomic_read(&s_state.network_available) &&
           aosl_atomic_read(&s_state.network_generation) == generation;
}

static void set_state(mybot_device_state_t new_state) {
    mybot_device_state_t old_state = current_state();
    if (old_state == new_state) {
        return;
    }

    /* Leaving a conversation invalidates any stop request generated while
     * that conversation was being torn down (for example by a synchronous
     * RTC SDK state callback fired from on_conversation_stop()). Otherwise the
     * next conversation would consume the stale request and stop immediately. */
    if (old_state == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        aosl_atomic_set(&s_state.stop_request, MYBOT_STOP_REQUEST_NONE);
    }

    aosl_atomic_set(&s_state.state, (intptr_t)new_state);
    AOSL_LOG_NTC("%s", s_name[new_state]);
    if (s_state.cbs.on_state_changed) {
        s_state.cbs.on_state_changed(new_state);
    }
}

static bool api_rejected_device_auth(int ret) {
    return ret == 401 || ret == 403 || ret == 409;
}

static int clamp_poll_interval(int seconds) {
    if (seconds < MYBOT_POLL_INTERVAL_MIN_SECONDS) {
        return MYBOT_POLL_INTERVAL_MIN_SECONDS;
    }
    if (seconds > MYBOT_POLL_INTERVAL_MAX_SECONDS) {
        return MYBOT_POLL_INTERVAL_MAX_SECONDS;
    }
    return seconds;
}

static int persist_device_auth(void) {
    mybot_device_auth_record_t record;
    memset(&record, 0, sizeof(record));
    record.version = MYBOT_DEVICE_AUTH_VERSION;
    strncpy(record.server_base, s_state.server_base, sizeof(record.server_base) - 1);
    strncpy(record.device_id, s_state.device_id, sizeof(record.device_id) - 1);
    strncpy(record.device_token, s_state.device_token, sizeof(record.device_token) - 1);
    return mybot_kv_store_set(MYBOT_DEVICE_AUTH_KEY, &record, sizeof(record));
}

static bool load_device_auth(void) {
    mybot_device_auth_record_t record;
    size_t len = 0;
    memset(&record, 0, sizeof(record));

    int ret = mybot_kv_store_get(MYBOT_DEVICE_AUTH_KEY, &record, sizeof(record), &len);
    if (ret == MYBOT_ERR_NOT_FOUND) {
        return false;
    }
    if (ret < 0 || len != sizeof(record) || record.version != MYBOT_DEVICE_AUTH_VERSION ||
        record.server_base[sizeof(record.server_base) - 1] != '\0' ||
        record.device_id[sizeof(record.device_id) - 1] != '\0' ||
        record.device_token[sizeof(record.device_token) - 1] != '\0' ||
        strcmp(record.server_base, s_state.server_base) != 0 ||
        strcmp(record.device_id, s_state.device_id) != 0 || record.device_token[0] == '\0') {
        if (ret < 0) {
            AOSL_LOG_ERR("failed to read persisted device credential");
        }
        (void)mybot_kv_store_erase(MYBOT_DEVICE_AUTH_KEY);
        return false;
    }

    strncpy(s_state.device_token, record.device_token, sizeof(s_state.device_token) - 1);
    return true;
}

static void clear_device_auth(void) {
    s_state.device_token[0] = '\0';
    if (mybot_kv_store_erase(MYBOT_DEVICE_AUTH_KEY) < 0) {
        AOSL_LOG_ERR("failed to erase persisted device credential");
    }
}

static void restart_pairing_after_auth_rejection(void) {
    clear_device_auth();
    set_state(MYBOT_DEVICE_STATE_UNPROVISIONED);
    aosl_atomic_set(&s_state.start_pairing_flag, true);
}

const char *mybot_device_lifecycle_get_token(void) {
    return (current_state() == MYBOT_DEVICE_STATE_RUNTIME ||
            current_state() == MYBOT_DEVICE_STATE_IN_CONVERSATION)
               ? s_state.device_token
               : NULL;
}

mybot_device_state_t mybot_device_lifecycle_get_state(void) {
    return current_state();
}

/* ----------------------------------------------------------
 * Action: request a pair code
 * ---------------------------------------------------------- */
static void action_create_pair_code(void) {
    mybot_device_pair_code_t resp;
    memset(&resp, 0, sizeof(resp));

    intptr_t network_generation = aosl_atomic_read(&s_state.network_generation);
    if (!aosl_atomic_read(&s_state.network_available)) {
        set_state(MYBOT_DEVICE_STATE_UNPROVISIONED);
        aosl_atomic_set(&s_state.start_pairing_flag, true);
        return;
    }

    int ret = mybot_device_client_create_pair_code(s_state.server_base, s_state.device_id,
                                                   s_state.firmware_ver, s_state.hw_model, &resp);
    if (!network_request_is_current(network_generation)) {
        AOSL_LOG_WRN("discarding pair-code response after network change");
        set_state(MYBOT_DEVICE_STATE_UNPROVISIONED);
        aosl_atomic_set(&s_state.start_pairing_flag, true);
        return;
    }
    if (ret != 0) {
        if (s_state.pair_retry_delay_ticks == 0) {
            s_state.pair_retry_delay_ticks = MYBOT_PAIR_RETRY_INITIAL_TICKS;
        } else if (s_state.pair_retry_delay_ticks < MYBOT_PAIR_RETRY_MAX_TICKS / 2) {
            s_state.pair_retry_delay_ticks *= 2;
        } else {
            s_state.pair_retry_delay_ticks = MYBOT_PAIR_RETRY_MAX_TICKS;
        }
        s_state.pair_retry_ticks_remaining = s_state.pair_retry_delay_ticks;
        AOSL_LOG_ERR("pair-code request failed, retrying in %d seconds",
                     s_state.pair_retry_delay_ticks / 10);
        set_state(MYBOT_DEVICE_STATE_UNPROVISIONED);
        return;
    }

    s_state.pair_retry_delay_ticks = 0;
    s_state.pair_retry_ticks_remaining = 0;

    AOSL_LOG_NTC("pair-code obtained: code=%s, poll=%ds", resp.code, resp.poll_after_seconds);

    /* Save pair token and poll settings */
    strncpy(s_state.pair_token, resp.pair_token, sizeof(s_state.pair_token) - 1);
    s_state.pair_poll_interval = clamp_poll_interval(resp.poll_after_seconds);
    s_state.pair_tick_counter = 0;

    /* Clear any old device token */
    s_state.device_token[0] = '\0';

    /* Notify the app to present the pair code. */
    if (s_state.cbs.on_pair_code) {
        s_state.cbs.on_pair_code(resp.code);
    }

    set_state(MYBOT_DEVICE_STATE_AWAITING_CLAIM);
}

/* ----------------------------------------------------------
 * Action: check claim status during pairing
 * ---------------------------------------------------------- */
static void action_poll_binding_pair(void) {
    char auth[MYBOT_DEVICE_CLIENT_MAX_TOKEN + 16];
    snprintf(auth, sizeof(auth), "Pair %s", s_state.pair_token);

    mybot_device_binding_t resp;
    memset(&resp, 0, sizeof(resp));

    intptr_t network_generation = aosl_atomic_read(&s_state.network_generation);
    if (!aosl_atomic_read(&s_state.network_available)) {
        return;
    }

    int ret =
        mybot_device_client_get_binding_status(s_state.server_base, s_state.device_id, auth, &resp);
    if (!network_request_is_current(network_generation)) {
        AOSL_LOG_WRN("discarding pair-status response after network change");
        return;
    }
    if (api_rejected_device_auth(ret)) {
        AOSL_LOG_WRN("pair credential rejected (HTTP %d), requesting a new pair code", ret);
        aosl_atomic_set(&s_state.start_pairing_flag, true);
        return;
    }
    if (ret != 0) {
        AOSL_LOG_ERR("bind poll (pair) failed, retrying");
        return;
    }

    AOSL_LOG_NTC("bind poll -> status=%s", resp.status);

    if (strcmp(resp.status, "pending") == 0) {
        s_state.pair_poll_interval = clamp_poll_interval(resp.poll_after_seconds);
        /* Stay in awaiting_claim. */
    } else if (strcmp(resp.status, "bound") == 0) {
        if (resp.device_token[0]) {
            strncpy(s_state.device_token, resp.device_token, sizeof(s_state.device_token) - 1);
        }
        if (!s_state.device_token[0]) {
            AOSL_LOG_ERR("bound response did not include the one-time device credential");
            return;
        }
        if (persist_device_auth() < 0) {
            AOSL_LOG_ERR("failed to persist device credential, retrying");
            return;
        }
        AOSL_LOG_NTC("device credential persisted");
        set_state(MYBOT_DEVICE_STATE_RUNTIME);
        s_state.runtime_poll_interval = clamp_poll_interval(resp.poll_after_seconds);
        s_state.runtime_tick_counter = 0;
    } else if (strcmp(resp.status, "expired") == 0 || strcmp(resp.status, "failed") == 0) {
        AOSL_LOG_NTC("pairing status %s, re-pairing", resp.status);
        /* The top-level pairing handler in tick() re-runs the pair-code
         * request on the next tick. */
        aosl_atomic_set(&s_state.start_pairing_flag, true);
    } else if (strcmp(resp.status, "unbound") == 0) {
        /* Unexpected during pairing, but handle it gracefully. */
        AOSL_LOG_NTC("unexpected unbound during pairing");
        set_state(MYBOT_DEVICE_STATE_UNPROVISIONED);
    } else {
        /* Unknown status. */
        AOSL_LOG_ERR("unknown bind status: %s", resp.status);
    }
}

/* ----------------------------------------------------------
 * Action: check binding status during runtime
 * ---------------------------------------------------------- */
static void complete_conversation_locally(void);

static void invalidate_runtime_binding(void) {
    if (current_state() == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        complete_conversation_locally();
    }
    restart_pairing_after_auth_rejection();
}

static void action_poll_binding_runtime(void) {
    char auth[MYBOT_DEVICE_CLIENT_MAX_TOKEN + 16];
    snprintf(auth, sizeof(auth), "Device %s", s_state.device_token);

    mybot_device_binding_t resp;
    memset(&resp, 0, sizeof(resp));

    intptr_t network_generation = aosl_atomic_read(&s_state.network_generation);
    if (!aosl_atomic_read(&s_state.network_available)) {
        return;
    }

    int ret =
        mybot_device_client_get_binding_status(s_state.server_base, s_state.device_id, auth, &resp);
    if (!network_request_is_current(network_generation)) {
        AOSL_LOG_WRN("discarding runtime-status response after network change");
        return;
    }
    if (api_rejected_device_auth(ret)) {
        AOSL_LOG_WRN("device credential rejected (HTTP %d), re-pairing", ret);
        invalidate_runtime_binding();
        return;
    }
    if (ret != 0) {
        AOSL_LOG_ERR("bind poll (device) failed, retrying");
        return;
    }

    if (strcmp(resp.status, "bound") == 0) {
        s_state.runtime_poll_interval = clamp_poll_interval(resp.poll_after_seconds);
    } else if (strcmp(resp.status, "unbound") == 0) {
        AOSL_LOG_NTC("device unbound by user");
        invalidate_runtime_binding();
    } else {
        AOSL_LOG_ERR("unexpected runtime status: %s", resp.status);
    }
}

static void tick_runtime_binding_poll(void) {
    s_state.runtime_tick_counter++;
    int interval_ticks = s_state.runtime_poll_interval * 10;
    if (s_state.runtime_tick_counter >= interval_ticks) {
        s_state.runtime_tick_counter = 0;
        action_poll_binding_runtime();
    }
}

static void clear_rtc_token_renewal(void) {
    aosl_atomic_set(&s_state.rtc_token_renewal_requested, false);
    s_state.rtc_token_renewal_pending = false;
    s_state.rtc_token_retry_delay_ticks = 0;
    s_state.rtc_token_retry_ticks_remaining = 0;
    s_state.rtc_channel[0] = '\0';
    s_state.rtc_uid[0] = '\0';
}

static void schedule_rtc_token_retry(void) {
    if (s_state.rtc_token_retry_delay_ticks == 0) {
        s_state.rtc_token_retry_delay_ticks = MYBOT_RTC_TOKEN_RETRY_INITIAL_TICKS;
    } else if (s_state.rtc_token_retry_delay_ticks < MYBOT_RTC_TOKEN_RETRY_MAX_TICKS / 2) {
        s_state.rtc_token_retry_delay_ticks *= 2;
    } else {
        s_state.rtc_token_retry_delay_ticks = MYBOT_RTC_TOKEN_RETRY_MAX_TICKS;
    }
    s_state.rtc_token_retry_ticks_remaining = s_state.rtc_token_retry_delay_ticks;
    AOSL_LOG_WRN("RTC-token renewal failed, retrying in %d ms",
                 s_state.rtc_token_retry_delay_ticks * 100);
}

/* ----------------------------------------------------------
 * Action: start conversation
 * ---------------------------------------------------------- */
static void action_start_conversation(void) {
    mybot_device_conversation_t resp;
    memset(&resp, 0, sizeof(resp));

    intptr_t network_generation = aosl_atomic_read(&s_state.network_generation);
    if (!aosl_atomic_read(&s_state.network_available)) {
        return;
    }

    int ret = mybot_device_client_start_conversation(s_state.server_base, s_state.device_id,
                                                     s_state.device_token, NULL, &resp);
    if (!network_request_is_current(network_generation)) {
        AOSL_LOG_WRN("discarding conversation response after network change");
        return;
    }
    if (api_rejected_device_auth(ret)) {
        AOSL_LOG_WRN("device credential rejected while starting conversation (HTTP %d)", ret);
        restart_pairing_after_auth_rejection();
        return;
    }
    if (ret != 0) {
        AOSL_LOG_ERR("start conversation failed");
        return;
    }

    if (!memchr(resp.conversation_id, '\0', sizeof(resp.conversation_id)) ||
        resp.conversation_id[0] == '\0') {
        AOSL_LOG_ERR("start conversation returned an invalid conversation_id");
        return;
    }

    strncpy(s_state.conversation_id, resp.conversation_id, sizeof(s_state.conversation_id) - 1);
    clear_rtc_token_renewal();
    snprintf(s_state.rtc_channel, sizeof(s_state.rtc_channel), "%s", resp.rtc_channel);
    snprintf(s_state.rtc_uid, sizeof(s_state.rtc_uid), "%s", resp.rtc_uid);

    AOSL_LOG_NTC("conversation started: %s, channel=%s, uid=%s", s_state.conversation_id,
                 resp.rtc_channel, resp.rtc_uid);

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
static void complete_conversation_locally(void) {
    clear_rtc_token_renewal();
    s_state.conversation_id[0] = '\0';
    if (s_state.cbs.on_conversation_stop) {
        s_state.cbs.on_conversation_stop();
    }
    set_state(MYBOT_DEVICE_STATE_RUNTIME);
}

static void action_stop_conversation(const char *reason) {
    if (!s_state.conversation_id[0]) {
        AOSL_LOG_ERR("active conversation has no conversation_id; completing local cleanup");
        complete_conversation_locally();
        return;
    }

    intptr_t network_generation = aosl_atomic_read(&s_state.network_generation);
    if (!aosl_atomic_read(&s_state.network_available)) {
        complete_conversation_locally();
        return;
    }

    int ret = mybot_device_client_stop_conversation(s_state.server_base, s_state.device_id,
                                                    s_state.device_token, s_state.conversation_id,
                                                    reason);

    if (!network_request_is_current(network_generation)) {
        AOSL_LOG_WRN("discarding stop-conversation response after network change");
        complete_conversation_locally();
        return;
    }

    AOSL_LOG_NTC("conversation stopped");
    clear_rtc_token_renewal();
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

static void action_renew_rtc_token(void) {
    mybot_device_rtc_token_t resp;
    memset(&resp, 0, sizeof(resp));

    if (!s_state.rtc_channel[0] || !s_state.rtc_uid[0]) {
        AOSL_LOG_ERR("cannot renew RTC token without an active channel and UID");
        action_stop_conversation(MYBOT_CONVERSATION_STOP_REASON_ERROR);
        return;
    }

    intptr_t network_generation = aosl_atomic_read(&s_state.network_generation);
    int ret = mybot_device_client_renew_rtc_token(s_state.server_base, s_state.device_id,
                                                  s_state.device_token, s_state.rtc_channel,
                                                  s_state.rtc_uid, &resp);
    if (!network_request_is_current(network_generation)) {
        AOSL_LOG_WRN("discarding RTC-token response after network change");
        return;
    }
    if (api_rejected_device_auth(ret)) {
        AOSL_LOG_WRN("device credential rejected while renewing RTC token (HTTP %d)", ret);
        invalidate_runtime_binding();
        return;
    }
    if (ret == 400 || ret == 410) {
        AOSL_LOG_ERR("RTC-token renewal permanently rejected (HTTP %d)", ret);
        action_stop_conversation(MYBOT_CONVERSATION_STOP_REASON_ERROR);
        return;
    }
    if (ret != 0) {
        schedule_rtc_token_retry();
        return;
    }
    if (strcmp(resp.rtc_channel, s_state.rtc_channel) != 0 ||
        strcmp(resp.rtc_uid, s_state.rtc_uid) != 0) {
        AOSL_LOG_ERR("RTC-token response does not match the active channel and UID");
        action_stop_conversation(MYBOT_CONVERSATION_STOP_REASON_ERROR);
        return;
    }
    if (!s_state.cbs.on_rtc_token_renewed || s_state.cbs.on_rtc_token_renewed(resp.rtc_token) < 0) {
        AOSL_LOG_ERR("RTC SDK rejected renewed token");
        schedule_rtc_token_retry();
        return;
    }

    s_state.rtc_token_renewal_pending = false;
    s_state.rtc_token_retry_delay_ticks = 0;
    s_state.rtc_token_retry_ticks_remaining = 0;
    AOSL_LOG_NTC("RTC token renewed");
}

/* ----------------------------------------------------------
 * Public API
 * ---------------------------------------------------------- */

int mybot_device_lifecycle_init(const char *server_base, const char *device_id,
                                const char *firmware_ver, const char *hw_model,
                                mybot_device_lifecycle_callbacks_t *cbs) {
    if (!server_base || !device_id) {
        return -1;
    }

    memset(&s_state, 0, sizeof(s_state));
    aosl_atomic_set(&s_state.network_available, true);

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
        AOSL_LOG_NTC("restored persisted device credential");
    } else {
        set_state(MYBOT_DEVICE_STATE_UNPROVISIONED);
        aosl_atomic_set(&s_state.start_pairing_flag, true);
    }

    return 0;
}

void mybot_device_lifecycle_tick(void) {
    if (aosl_atomic_read(&s_state.shutting_down)) {
        return;
    }

    /* Consume every observed network loss even when Wi-Fi reconnects before
     * this worker gets its next tick. */
    if (aosl_atomic_xchg(&s_state.network_loss_pending, false)) {
        aosl_atomic_set(&s_state.conversation_requested, false);
        aosl_atomic_set(&s_state.stop_request, MYBOT_STOP_REQUEST_NONE);
        if (current_state() == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
            complete_conversation_locally();
        }
        return;
    }

    if (!aosl_atomic_read(&s_state.network_available)) {
        aosl_atomic_set(&s_state.conversation_requested, false);
        aosl_atomic_set(&s_state.stop_request, MYBOT_STOP_REQUEST_NONE);
        return;
    }

    /* A pending pairing request (first boot, expired pair code, or an explicit
     * re-pair request) starts a fresh pair-code request from any state. If a
     * conversation is active, end it first so the RTC connection is torn down
     * before the device is rebound. */
    if (aosl_atomic_xchg(&s_state.start_pairing_flag, false)) {
        if (current_state() == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
            action_stop_conversation(MYBOT_CONVERSATION_STOP_REASON_USER_REQUESTED);
        }
        clear_device_auth();
        s_state.conversation_id[0] = '\0';
        set_state(MYBOT_DEVICE_STATE_PAIRING);
        action_create_pair_code();
        return;
    }

    if (current_state() == MYBOT_DEVICE_STATE_UNPROVISIONED) {
        if (s_state.pair_retry_ticks_remaining > 0) {
            s_state.pair_retry_ticks_remaining--;
            if (s_state.pair_retry_ticks_remaining == 0) {
                set_state(MYBOT_DEVICE_STATE_PAIRING);
                action_create_pair_code();
            }
        }
        return;
    }

    if (current_state() == MYBOT_DEVICE_STATE_PAIRING) {
        /* This state is transient — action_create_pair_code() moves out */
        return;
    }

    if (current_state() == MYBOT_DEVICE_STATE_AWAITING_CLAIM) {
        s_state.pair_tick_counter++;
        /* Convert the poll interval using the 100 ms tick period. */
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

        tick_runtime_binding_poll();
        return;
    }

    if (current_state() == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        mybot_stop_request_t request =
            (mybot_stop_request_t)aosl_atomic_xchg(&s_state.stop_request, MYBOT_STOP_REQUEST_NONE);
        if (request != MYBOT_STOP_REQUEST_NONE) {
            const char *reason = request == MYBOT_STOP_REQUEST_DEVICE_HANGUP
                                     ? MYBOT_CONVERSATION_STOP_REASON_DEVICE_HANGUP
                                     : MYBOT_CONVERSATION_STOP_REASON_ERROR;
            action_stop_conversation(reason);
            return;
        }

        if (aosl_atomic_xchg(&s_state.rtc_token_renewal_requested, false)) {
            s_state.rtc_token_renewal_pending = true;
            s_state.rtc_token_retry_delay_ticks = 0;
            s_state.rtc_token_retry_ticks_remaining = 0;
        }
        if (s_state.rtc_token_renewal_pending) {
            if (s_state.rtc_token_retry_ticks_remaining > 0) {
                s_state.rtc_token_retry_ticks_remaining--;
            }
            if (s_state.rtc_token_retry_ticks_remaining == 0) {
                action_renew_rtc_token();
                return;
            }
        }
        tick_runtime_binding_poll();
        return;
    }
}

void mybot_device_lifecycle_set_network_available(bool available) {
    aosl_atomic_set(&s_state.network_available, available);
    if (!available) {
        aosl_atomic_inc(&s_state.network_generation);
        aosl_atomic_set(&s_state.network_loss_pending, true);
        /* Do not start a conversation automatically after a later reconnect. */
        aosl_atomic_set(&s_state.conversation_requested, false);
    }
}

void mybot_device_lifecycle_shutdown(void) {
    aosl_atomic_set(&s_state.shutting_down, true);
    aosl_atomic_set(&s_state.start_pairing_flag, false);
    aosl_atomic_set(&s_state.conversation_requested, false);
    aosl_atomic_set(&s_state.stop_request, MYBOT_STOP_REQUEST_NONE);
    aosl_atomic_set(&s_state.rtc_token_renewal_requested, false);

    if (current_state() == MYBOT_DEVICE_STATE_IN_CONVERSATION &&
        aosl_atomic_read(&s_state.network_available) &&
        !aosl_atomic_read(&s_state.network_loss_pending)) {
        action_stop_conversation(MYBOT_CONVERSATION_STOP_REASON_DEVICE_HANGUP);
    } else if (current_state() == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        complete_conversation_locally();
    }
}

void mybot_device_lifecycle_request_pair(void) {
    if (aosl_atomic_read(&s_state.shutting_down)) {
        return;
    }
    aosl_atomic_set(&s_state.start_pairing_flag, true);
}

void mybot_device_lifecycle_request_start(void) {
    if (aosl_atomic_read(&s_state.shutting_down) || !aosl_atomic_read(&s_state.network_available)) {
        return;
    }
    if (current_state() != MYBOT_DEVICE_STATE_RUNTIME) {
        AOSL_LOG_ERR("cannot start: not in runtime");
        return;
    }
    aosl_atomic_set(&s_state.conversation_requested, true);
}

void mybot_device_lifecycle_request_stop(void) {
    if (aosl_atomic_read(&s_state.shutting_down)) {
        return;
    }
    if (current_state() != MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        AOSL_LOG_ERR("cannot stop: not in conversation");
        return;
    }
    aosl_atomic_set(&s_state.stop_request, MYBOT_STOP_REQUEST_DEVICE_HANGUP);
}

void mybot_device_lifecycle_notify_conversation_ended(void) {
    if (aosl_atomic_read(&s_state.shutting_down)) {
        return;
    }
    /* Called from an RTC SDK callback thread on connection loss/error. Only
     * flag the stop here — the actual teardown (HTTP stop + RTC leave) runs
     * on the state_mpq thread via mybot_device_lifecycle_tick(), avoiding
     * re-entrant SDK calls from inside an SDK callback. */
    if (current_state() == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        aosl_atomic_set(&s_state.stop_request, MYBOT_STOP_REQUEST_ERROR);
    }
}

void mybot_device_lifecycle_request_rtc_token_renewal(void) {
    if (aosl_atomic_read(&s_state.shutting_down) || !aosl_atomic_read(&s_state.network_available)) {
        return;
    }
    if (current_state() != MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        AOSL_LOG_WRN("ignoring RTC-token renewal request without an active conversation");
        return;
    }
    aosl_atomic_set(&s_state.rtc_token_renewal_requested, true);
}
