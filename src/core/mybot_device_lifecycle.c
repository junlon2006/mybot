/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_device_lifecycle.h"
#include "mybot_device_client.h"
#include <mybot/platform/mybot_kv_store.h>

#include "mybot_kv_store_internal.h"

#include <api/aosl_log.h>
#include <api/aosl_atomic.h>
#include <hal/aosl_hal_memory.h>

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

static mybot_device_state_t current_state(const mybot_device_lifecycle_t *lifecycle) {
    return (mybot_device_state_t)aosl_atomic_read(&lifecycle->state);
}

static bool network_request_is_current(const mybot_device_lifecycle_t *lifecycle,
                                       intptr_t generation) {
    return aosl_atomic_read(&lifecycle->network_available) &&
           aosl_atomic_read(&lifecycle->network_generation) == generation;
}

static void set_state(mybot_device_lifecycle_t *lifecycle, mybot_device_state_t new_state) {
    mybot_device_state_t old_state = current_state(lifecycle);
    if (old_state == new_state) {
        return;
    }

    /* Leaving a conversation invalidates any stop request generated while
     * that conversation was being torn down (for example by a synchronous
     * RTC SDK state callback fired from on_conversation_stop()). Otherwise the
     * next conversation would consume the stale request and stop immediately. */
    if (old_state == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        aosl_atomic_set(&lifecycle->stop_request, MYBOT_STOP_REQUEST_NONE);
    }

    aosl_atomic_set(&lifecycle->state, (intptr_t)new_state);
    AOSL_LOG_NTC("%s", s_name[new_state]);
    if (lifecycle->cbs.on_state_changed) {
        lifecycle->cbs.on_state_changed(new_state, lifecycle->cbs.user_data);
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

static int next_retry_delay_ticks(int current, int initial, int maximum) {
    if (current == 0) {
        return initial;
    }
    return current < maximum / 2 ? current * 2 : maximum;
}

static int persist_device_auth(mybot_device_lifecycle_t *lifecycle) {
    mybot_device_auth_record_t *record =
        (mybot_device_auth_record_t *)aosl_hal_malloc(sizeof(*record));
    if (!record) {
        return -1;
    }
    memset(record, 0, sizeof(*record));
    record->version = MYBOT_DEVICE_AUTH_VERSION;
    strncpy(record->server_base, lifecycle->server_base, sizeof(record->server_base) - 1);
    strncpy(record->device_id, lifecycle->device_id, sizeof(record->device_id) - 1);
    strncpy(record->device_token, lifecycle->device_token, sizeof(record->device_token) - 1);
    int ret =
        mybot_kv_store_set(lifecycle->kv_store, MYBOT_DEVICE_AUTH_KEY, record, sizeof(*record));
    aosl_hal_free(record);
    return ret;
}

/* A missing or invalid record leaves device_token empty so pairing can begin. */
static int load_device_auth(mybot_device_lifecycle_t *lifecycle) {
    mybot_device_auth_record_t *record =
        (mybot_device_auth_record_t *)aosl_hal_malloc(sizeof(*record));
    size_t len = 0;
    if (!record) {
        AOSL_LOG_ERR("failed to allocate persisted device credential");
        return -1;
    }
    memset(record, 0, sizeof(*record));

    int ret = mybot_kv_store_get(lifecycle->kv_store, MYBOT_DEVICE_AUTH_KEY, record,
                                 sizeof(*record), &len);
    if (ret != 0) {
        aosl_hal_free(record);
        if (ret == MYBOT_ERR_NOT_FOUND) {
            return 0;
        }
        AOSL_LOG_ERR("failed to read persisted device credential");
        return -1;
    }
    if (len != sizeof(*record) || record->version != MYBOT_DEVICE_AUTH_VERSION ||
        record->server_base[sizeof(record->server_base) - 1] != '\0' ||
        record->device_id[sizeof(record->device_id) - 1] != '\0' ||
        record->device_token[sizeof(record->device_token) - 1] != '\0' ||
        strcmp(record->server_base, lifecycle->server_base) != 0 ||
        strcmp(record->device_id, lifecycle->device_id) != 0 || record->device_token[0] == '\0') {
        (void)mybot_kv_store_erase(lifecycle->kv_store, MYBOT_DEVICE_AUTH_KEY);
    } else {
        strncpy(lifecycle->device_token, record->device_token, sizeof(lifecycle->device_token) - 1);
    }
    aosl_hal_free(record);
    return 0;
}

static void clear_device_auth(mybot_device_lifecycle_t *lifecycle) {
    lifecycle->binding_generation++;
    lifecycle->device_token[0] = '\0';
    if (mybot_kv_store_erase(lifecycle->kv_store, MYBOT_DEVICE_AUTH_KEY) < 0) {
        AOSL_LOG_ERR("failed to erase persisted device credential");
    }
}

static void restart_pairing_after_auth_rejection(mybot_device_lifecycle_t *lifecycle) {
    clear_device_auth(lifecycle);
    set_state(lifecycle, MYBOT_DEVICE_STATE_UNPROVISIONED);
    lifecycle->pairing_requested = true;
}

/* ----------------------------------------------------------
 * Action: request a pair code
 * ---------------------------------------------------------- */
static void action_create_pair_code(mybot_device_lifecycle_t *lifecycle) {
    mybot_device_pair_code_t resp;
    memset(&resp, 0, sizeof(resp));

    intptr_t network_generation = aosl_atomic_read(&lifecycle->network_generation);
    if (!aosl_atomic_read(&lifecycle->network_available)) {
        set_state(lifecycle, MYBOT_DEVICE_STATE_UNPROVISIONED);
        lifecycle->pairing_requested = true;
        return;
    }

    int ret =
        mybot_device_client_create_pair_code(lifecycle->server_base, lifecycle->device_id,
                                             lifecycle->firmware_ver, lifecycle->hw_model, &resp);
    if (!network_request_is_current(lifecycle, network_generation)) {
        AOSL_LOG_WRN("discarding pair-code response after network change");
        set_state(lifecycle, MYBOT_DEVICE_STATE_UNPROVISIONED);
        lifecycle->pairing_requested = true;
        return;
    }
    if (ret != 0) {
        lifecycle->pair_retry_delay_ticks =
            next_retry_delay_ticks(lifecycle->pair_retry_delay_ticks,
                                   MYBOT_PAIR_RETRY_INITIAL_TICKS, MYBOT_PAIR_RETRY_MAX_TICKS);
        lifecycle->pair_retry_ticks_remaining = lifecycle->pair_retry_delay_ticks;
        AOSL_LOG_ERR("pair-code request failed, retrying in %d seconds",
                     lifecycle->pair_retry_delay_ticks / 10);
        set_state(lifecycle, MYBOT_DEVICE_STATE_UNPROVISIONED);
        return;
    }

    lifecycle->pair_retry_delay_ticks = 0;
    lifecycle->pair_retry_ticks_remaining = 0;

    AOSL_LOG_NTC("pair-code obtained: code=%s, poll=%ds", resp.code, resp.poll_after_seconds);

    /* Save pair token and poll settings */
    lifecycle->binding_generation++;
    strncpy(lifecycle->pair_token, resp.pair_token, sizeof(lifecycle->pair_token) - 1);
    lifecycle->pair_poll_interval = clamp_poll_interval(resp.poll_after_seconds);
    lifecycle->pair_tick_counter = 0;

    /* Clear any old device token */
    lifecycle->device_token[0] = '\0';

    /* Notify the app to present the pair code. */
    if (lifecycle->cbs.on_pair_code) {
        lifecycle->cbs.on_pair_code(resp.code, lifecycle->cbs.user_data);
    }

    set_state(lifecycle, MYBOT_DEVICE_STATE_AWAITING_CLAIM);
}

/* ----------------------------------------------------------
 * Background service request snapshots
 * ---------------------------------------------------------- */
static mybot_device_service_request_t *
create_service_request(const mybot_device_lifecycle_t *lifecycle,
                       mybot_device_service_request_type_t type) {
    mybot_device_service_request_t *request = aosl_hal_malloc(sizeof(*request));
    if (!request) {
        AOSL_LOG_ERR("failed to allocate background service request");
        return NULL;
    }
    memset(request, 0, sizeof(*request));
    request->type = type;
    request->network_generation = aosl_atomic_read(&lifecycle->network_generation);
    request->binding_generation = lifecycle->binding_generation;
    memcpy(request->server_base, lifecycle->server_base, sizeof(request->server_base));
    memcpy(request->device_id, lifecycle->device_id, sizeof(request->device_id));
    memcpy(request->credential,
           type == MYBOT_DEVICE_SERVICE_PAIR_POLL ? lifecycle->pair_token : lifecycle->device_token,
           sizeof(request->credential));
    return request;
}

static bool submit_service_request(mybot_device_lifecycle_t *lifecycle,
                                   mybot_device_service_request_t *request) {
    if (!lifecycle->cbs.submit_service_request ||
        lifecycle->cbs.submit_service_request(request, lifecycle->cbs.user_data) < 0) {
        AOSL_LOG_ERR("failed to queue background service request (type=%d)", request->type);
        aosl_hal_free(request);
        return false;
    }
    return true;
}

static void action_poll_binding(mybot_device_lifecycle_t *lifecycle,
                                mybot_device_service_request_type_t type) {
    if (lifecycle->poll_pending) {
        return;
    }
    mybot_device_service_request_t *request = create_service_request(lifecycle, type);
    if (!request) {
        return;
    }
    lifecycle->poll_pending = true;
    if (!submit_service_request(lifecycle, request)) {
        lifecycle->poll_pending = false;
    }
}

/* ----------------------------------------------------------
 * Apply claim status during pairing
 * ---------------------------------------------------------- */
static void apply_binding_pair(mybot_device_lifecycle_t *lifecycle,
                               const mybot_device_service_request_t *request) {
    int ret = request->result;
    const mybot_device_binding_t *resp = &request->payload.binding;
    if (api_rejected_device_auth(ret)) {
        AOSL_LOG_WRN("pair credential rejected (HTTP %d), requesting a new pair code", ret);
        lifecycle->pairing_requested = true;
        return;
    }
    if (ret != 0) {
        AOSL_LOG_ERR("bind poll (pair) failed, retrying");
        return;
    }

    AOSL_LOG_NTC("bind poll -> status=%s", resp->status);

    if (strcmp(resp->status, "pending") == 0) {
        lifecycle->pair_poll_interval = clamp_poll_interval(resp->poll_after_seconds);
        /* Stay in awaiting_claim. */
    } else if (strcmp(resp->status, "bound") == 0) {
        if (resp->device_token[0]) {
            lifecycle->binding_generation++;
            strncpy(lifecycle->device_token, resp->device_token,
                    sizeof(lifecycle->device_token) - 1);
        }
        if (!lifecycle->device_token[0]) {
            AOSL_LOG_ERR("bound response did not include the one-time device credential");
            return;
        }
        if (persist_device_auth(lifecycle) < 0) {
            AOSL_LOG_ERR("failed to persist device credential, retrying");
            return;
        }
        AOSL_LOG_NTC("device credential persisted");
        set_state(lifecycle, MYBOT_DEVICE_STATE_RUNTIME);
        lifecycle->runtime_poll_interval = clamp_poll_interval(resp->poll_after_seconds);
        lifecycle->runtime_tick_counter = 0;
    } else if (strcmp(resp->status, "expired") == 0 || strcmp(resp->status, "failed") == 0) {
        AOSL_LOG_NTC("pairing status %s, re-pairing", resp->status);
        /* The top-level pairing handler in tick() re-runs the pair-code
         * request on the next tick. */
        lifecycle->pairing_requested = true;
    } else if (strcmp(resp->status, "unbound") == 0) {
        /* Unexpected during pairing, but handle it gracefully. */
        AOSL_LOG_NTC("unexpected unbound during pairing");
        set_state(lifecycle, MYBOT_DEVICE_STATE_UNPROVISIONED);
        /* Re-enter the pairing flow on the next tick. */
        lifecycle->pairing_requested = true;
    } else {
        /* Unknown status. */
        AOSL_LOG_ERR("unknown bind status: %s", resp->status);
    }
}

/* ----------------------------------------------------------
 * Action: check binding status during runtime
 * ---------------------------------------------------------- */
static void complete_conversation_locally(mybot_device_lifecycle_t *lifecycle);

static void invalidate_runtime_binding(mybot_device_lifecycle_t *lifecycle) {
    if (current_state(lifecycle) == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        complete_conversation_locally(lifecycle);
    }
    restart_pairing_after_auth_rejection(lifecycle);
}

static void apply_binding_runtime(mybot_device_lifecycle_t *lifecycle,
                                  const mybot_device_service_request_t *request) {
    int ret = request->result;
    const mybot_device_binding_t *resp = &request->payload.binding;
    if (api_rejected_device_auth(ret)) {
        AOSL_LOG_WRN("device credential rejected (HTTP %d), re-pairing", ret);
        invalidate_runtime_binding(lifecycle);
        return;
    }
    if (ret != 0) {
        AOSL_LOG_ERR("bind poll (device) failed, retrying");
        return;
    }

    if (strcmp(resp->status, "bound") == 0) {
        lifecycle->runtime_poll_interval = clamp_poll_interval(resp->poll_after_seconds);
    } else if (strcmp(resp->status, "unbound") == 0) {
        AOSL_LOG_NTC("device unbound by user");
        invalidate_runtime_binding(lifecycle);
    } else {
        AOSL_LOG_ERR("unexpected runtime status: %s", resp->status);
    }
}

static void tick_runtime_binding_poll(mybot_device_lifecycle_t *lifecycle) {
    lifecycle->runtime_tick_counter++;
    int interval_ticks = lifecycle->runtime_poll_interval * 10;
    if (lifecycle->runtime_tick_counter >= interval_ticks) {
        lifecycle->runtime_tick_counter = 0;
        action_poll_binding(lifecycle, MYBOT_DEVICE_SERVICE_DEVICE_POLL);
    }
}

static void clear_rtc_token_renewal(mybot_device_lifecycle_t *lifecycle) {
    aosl_atomic_set(&lifecycle->rtc_token_renewal_requested, false);
    lifecycle->rtc_token_renewal_pending = false;
    lifecycle->rtc_token_retry_delay_ticks = 0;
    lifecycle->rtc_token_retry_ticks_remaining = 0;
    lifecycle->rtc_channel[0] = '\0';
    lifecycle->rtc_uid[0] = '\0';
}

static void schedule_rtc_token_retry(mybot_device_lifecycle_t *lifecycle) {
    lifecycle->rtc_token_retry_delay_ticks = next_retry_delay_ticks(
        lifecycle->rtc_token_retry_delay_ticks, MYBOT_RTC_TOKEN_RETRY_INITIAL_TICKS,
        MYBOT_RTC_TOKEN_RETRY_MAX_TICKS);
    lifecycle->rtc_token_retry_ticks_remaining = lifecycle->rtc_token_retry_delay_ticks;
    AOSL_LOG_WRN("RTC-token renewal failed, retrying in %d ms",
                 lifecycle->rtc_token_retry_delay_ticks * 100);
}

/* ----------------------------------------------------------
 * Action: start conversation
 * ---------------------------------------------------------- */
static void action_start_conversation(mybot_device_lifecycle_t *lifecycle) {
    mybot_device_conversation_t *resp =
        (mybot_device_conversation_t *)aosl_hal_malloc(sizeof(*resp));
    if (!resp) {
        AOSL_LOG_ERR("failed to allocate conversation response");
        return;
    }
    memset(resp, 0, sizeof(*resp));

    intptr_t network_generation = aosl_atomic_read(&lifecycle->network_generation);
    if (!aosl_atomic_read(&lifecycle->network_available)) {
        aosl_hal_free(resp);
        return;
    }

    int ret = mybot_device_client_start_conversation(lifecycle->server_base, lifecycle->device_id,
                                                     lifecycle->device_token, resp);
    if (!network_request_is_current(lifecycle, network_generation)) {
        AOSL_LOG_WRN("discarding conversation response after network change");
        aosl_hal_free(resp);
        return;
    }
    if (api_rejected_device_auth(ret)) {
        AOSL_LOG_WRN("device credential rejected while starting conversation (HTTP %d)", ret);
        restart_pairing_after_auth_rejection(lifecycle);
        aosl_hal_free(resp);
        return;
    }
    if (ret != 0) {
        AOSL_LOG_ERR("start conversation failed");
        aosl_hal_free(resp);
        return;
    }

    if (!memchr(resp->conversation_id, '\0', sizeof(resp->conversation_id)) ||
        resp->conversation_id[0] == '\0') {
        AOSL_LOG_ERR("start conversation returned an invalid conversation_id");
        aosl_hal_free(resp);
        return;
    }

    strncpy(lifecycle->conversation_id, resp->conversation_id,
            sizeof(lifecycle->conversation_id) - 1);
    clear_rtc_token_renewal(lifecycle);
    snprintf(lifecycle->rtc_channel, sizeof(lifecycle->rtc_channel), "%s", resp->rtc_channel);
    snprintf(lifecycle->rtc_uid, sizeof(lifecycle->rtc_uid), "%s", resp->rtc_uid);

    AOSL_LOG_NTC("conversation started: %s, channel=%s, uid=%s", lifecycle->conversation_id,
                 resp->rtc_channel, resp->rtc_uid);

    set_state(lifecycle, MYBOT_DEVICE_STATE_IN_CONVERSATION);

    /* The app borrows this response only for the synchronous callback. */
    if (lifecycle->cbs.on_conversation_start) {
        lifecycle->cbs.on_conversation_start(resp, lifecycle->cbs.user_data);
    }
    aosl_hal_free(resp);
}

/* ----------------------------------------------------------
 * Action: stop conversation
 * ---------------------------------------------------------- */
static void complete_conversation_locally(mybot_device_lifecycle_t *lifecycle) {
    clear_rtc_token_renewal(lifecycle);
    lifecycle->conversation_id[0] = '\0';
    if (lifecycle->cbs.on_conversation_stop) {
        lifecycle->cbs.on_conversation_stop(lifecycle->cbs.user_data);
    }
    set_state(lifecycle, MYBOT_DEVICE_STATE_RUNTIME);
}

static void action_stop_conversation(mybot_device_lifecycle_t *lifecycle, const char *reason) {
    char conversation_id[MYBOT_DEVICE_CLIENT_MAX_ID];
    memcpy(conversation_id, lifecycle->conversation_id, sizeof(conversation_id));
    intptr_t network_generation = aosl_atomic_read(&lifecycle->network_generation);

    /* Finish RTC/media teardown and publish the local state before queuing HTTP. */
    complete_conversation_locally(lifecycle);
    if (!conversation_id[0]) {
        AOSL_LOG_ERR("stop notification skipped: missing conversation_id; local session stopped");
        return;
    }
    if (!network_request_is_current(lifecycle, network_generation)) {
        return;
    }

    mybot_device_service_request_t *request =
        create_service_request(lifecycle, MYBOT_DEVICE_SERVICE_STOP);
    if (!request) {
        return;
    }
    memcpy(request->payload.stop.conversation_id, conversation_id, sizeof(conversation_id));
    snprintf(request->payload.stop.reason, sizeof(request->payload.stop.reason), "%s", reason);
    /* The server also ends the session when the device leaves RTC; notify it once. */
    (void)submit_service_request(lifecycle, request);
}

static void apply_stop_result(mybot_device_lifecycle_t *lifecycle, int ret) {
    if (api_rejected_device_auth(ret)) {
        AOSL_LOG_WRN("device credential rejected while stopping conversation (HTTP %d)", ret);
        invalidate_runtime_binding(lifecycle);
        return;
    }

    if (ret != 0) {
        AOSL_LOG_WRN("stop-conversation notification failed (%d); local session already stopped",
                     ret);
        return;
    }

    AOSL_LOG_NTC("conversation stop notified");
}

static void action_renew_rtc_token(mybot_device_lifecycle_t *lifecycle) {
    mybot_device_rtc_token_t resp;
    memset(&resp, 0, sizeof(resp));

    if (!lifecycle->rtc_channel[0] || !lifecycle->rtc_uid[0]) {
        AOSL_LOG_ERR("cannot renew RTC token without an active channel and UID");
        action_stop_conversation(lifecycle, MYBOT_CONVERSATION_STOP_REASON_ERROR);
        return;
    }

    intptr_t network_generation = aosl_atomic_read(&lifecycle->network_generation);
    int ret = mybot_device_client_renew_rtc_token(lifecycle->server_base, lifecycle->device_id,
                                                  lifecycle->device_token, lifecycle->rtc_channel,
                                                  lifecycle->rtc_uid, &resp);
    if (!network_request_is_current(lifecycle, network_generation)) {
        AOSL_LOG_WRN("discarding RTC-token response after network change");
        return;
    }
    if (api_rejected_device_auth(ret)) {
        AOSL_LOG_WRN("device credential rejected while renewing RTC token (HTTP %d)", ret);
        invalidate_runtime_binding(lifecycle);
        return;
    }
    if (ret == 400 || ret == 410) {
        AOSL_LOG_ERR("RTC-token renewal permanently rejected (HTTP %d)", ret);
        action_stop_conversation(lifecycle, MYBOT_CONVERSATION_STOP_REASON_ERROR);
        return;
    }
    if (ret != 0) {
        schedule_rtc_token_retry(lifecycle);
        return;
    }
    if (strcmp(resp.rtc_channel, lifecycle->rtc_channel) != 0 ||
        strcmp(resp.rtc_uid, lifecycle->rtc_uid) != 0) {
        AOSL_LOG_ERR("RTC-token response does not match the active channel and UID");
        action_stop_conversation(lifecycle, MYBOT_CONVERSATION_STOP_REASON_ERROR);
        return;
    }
    if (!lifecycle->cbs.on_rtc_token_renewed ||
        lifecycle->cbs.on_rtc_token_renewed(resp.rtc_token, lifecycle->cbs.user_data) < 0) {
        AOSL_LOG_ERR("RTC SDK rejected renewed token");
        schedule_rtc_token_retry(lifecycle);
        return;
    }

    lifecycle->rtc_token_renewal_pending = false;
    lifecycle->rtc_token_retry_delay_ticks = 0;
    lifecycle->rtc_token_retry_ticks_remaining = 0;
    AOSL_LOG_NTC("RTC token renewed");
}

/* ----------------------------------------------------------
 * Public API
 * ---------------------------------------------------------- */

int mybot_device_lifecycle_init(mybot_device_lifecycle_t *lifecycle, mybot_kv_store_t *kv_store,
                                const char *server_base, const char *device_id,
                                const char *firmware_ver, const char *hw_model,
                                const mybot_device_lifecycle_callbacks_t *cbs) {
    if (!lifecycle || !kv_store || !server_base || !device_id) {
        return -1;
    }
    if (!server_base[0] || !device_id[0] ||
        !memchr(server_base, '\0', sizeof(lifecycle->server_base)) ||
        !memchr(device_id, '\0', sizeof(lifecycle->device_id)) ||
        (firmware_ver && !memchr(firmware_ver, '\0', sizeof(lifecycle->firmware_ver))) ||
        (hw_model && !memchr(hw_model, '\0', sizeof(lifecycle->hw_model)))) {
        AOSL_LOG_ERR("lifecycle init rejected: configuration string is too long or empty");
        return -1;
    }

    memset(lifecycle, 0, sizeof(*lifecycle));
    lifecycle->kv_store = kv_store;
    aosl_atomic_set(&lifecycle->network_available, true);

    strncpy(lifecycle->server_base, server_base, sizeof(lifecycle->server_base) - 1);
    strncpy(lifecycle->device_id, device_id, sizeof(lifecycle->device_id) - 1);
    if (firmware_ver) {
        strncpy(lifecycle->firmware_ver, firmware_ver, sizeof(lifecycle->firmware_ver) - 1);
    }
    if (hw_model) {
        strncpy(lifecycle->hw_model, hw_model, sizeof(lifecycle->hw_model) - 1);
    }
    if (cbs) {
        lifecycle->cbs = *cbs;
    }

    if (load_device_auth(lifecycle) < 0) {
        return -1;
    }
    if (lifecycle->device_token[0]) {
        lifecycle->runtime_poll_interval = 30;
        set_state(lifecycle, MYBOT_DEVICE_STATE_RUNTIME);
        AOSL_LOG_NTC("restored persisted device credential");
    } else {
        set_state(lifecycle, MYBOT_DEVICE_STATE_UNPROVISIONED);
        lifecycle->pairing_requested = true;
    }

    return 0;
}

void mybot_device_lifecycle_service_result(mybot_device_lifecycle_t *lifecycle,
                                           const mybot_device_service_request_t *request) {
    if (request->type != MYBOT_DEVICE_SERVICE_STOP) {
        /* Keep one poll outstanding across reconnect/re-pair until its owner returns it. */
        lifecycle->poll_pending = false;
    }
    if (aosl_atomic_read(&lifecycle->shutting_down) ||
        !network_request_is_current(lifecycle, request->network_generation) ||
        lifecycle->binding_generation != request->binding_generation ||
        lifecycle->pairing_requested) {
        AOSL_LOG_DBG("discarding stale background service result (type=%d)", request->type);
        return;
    }

    mybot_device_state_t state = current_state(lifecycle);
    if (request->type == MYBOT_DEVICE_SERVICE_PAIR_POLL &&
        state == MYBOT_DEVICE_STATE_AWAITING_CLAIM) {
        apply_binding_pair(lifecycle, request);
    } else if (state == MYBOT_DEVICE_STATE_RUNTIME || state == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        if (request->type == MYBOT_DEVICE_SERVICE_DEVICE_POLL) {
            apply_binding_runtime(lifecycle, request);
        } else if (request->type == MYBOT_DEVICE_SERVICE_STOP) {
            apply_stop_result(lifecycle, request->result);
        }
    }
}

static void tick_conversation(mybot_device_lifecycle_t *lifecycle) {
    mybot_stop_request_t request =
        (mybot_stop_request_t)aosl_atomic_xchg(&lifecycle->stop_request, MYBOT_STOP_REQUEST_NONE);
    if (request != MYBOT_STOP_REQUEST_NONE) {
        const char *reason = request == MYBOT_STOP_REQUEST_DEVICE_HANGUP
                                 ? MYBOT_CONVERSATION_STOP_REASON_DEVICE_HANGUP
                                 : MYBOT_CONVERSATION_STOP_REASON_ERROR;
        action_stop_conversation(lifecycle, reason);
        return;
    }

    if (aosl_atomic_xchg(&lifecycle->rtc_token_renewal_requested, false)) {
        lifecycle->rtc_token_renewal_pending = true;
        lifecycle->rtc_token_retry_delay_ticks = 0;
        lifecycle->rtc_token_retry_ticks_remaining = 0;
    }
    if (lifecycle->rtc_token_renewal_pending) {
        if (lifecycle->rtc_token_retry_ticks_remaining > 0) {
            lifecycle->rtc_token_retry_ticks_remaining--;
        }
        if (lifecycle->rtc_token_retry_ticks_remaining == 0) {
            action_renew_rtc_token(lifecycle);
            return;
        }
    }
    tick_runtime_binding_poll(lifecycle);
}

static void begin_pairing(mybot_device_lifecycle_t *lifecycle) {
    lifecycle->conversation_requested = false;
    if (current_state(lifecycle) == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        action_stop_conversation(lifecycle, MYBOT_CONVERSATION_STOP_REASON_USER_REQUESTED);
    }
    lifecycle->pairing_requested = false;
    clear_device_auth(lifecycle);
    lifecycle->conversation_id[0] = '\0';
    set_state(lifecycle, MYBOT_DEVICE_STATE_PAIRING);
    action_create_pair_code(lifecycle);
}

static void tick_device_state(mybot_device_lifecycle_t *lifecycle) {
    switch (current_state(lifecycle)) {
    case MYBOT_DEVICE_STATE_UNPROVISIONED:
        if (lifecycle->pair_retry_ticks_remaining > 0 &&
            --lifecycle->pair_retry_ticks_remaining == 0) {
            set_state(lifecycle, MYBOT_DEVICE_STATE_PAIRING);
            action_create_pair_code(lifecycle);
        }
        break;
    case MYBOT_DEVICE_STATE_PAIRING:
        /* The synchronous pair-code request completes this transition. */
        break;
    case MYBOT_DEVICE_STATE_AWAITING_CLAIM:
        lifecycle->pair_tick_counter++;
        if (lifecycle->pair_tick_counter >= lifecycle->pair_poll_interval * 10) {
            lifecycle->pair_tick_counter = 0;
            action_poll_binding(lifecycle, MYBOT_DEVICE_SERVICE_PAIR_POLL);
        }
        break;
    case MYBOT_DEVICE_STATE_RUNTIME:
        if (lifecycle->conversation_requested) {
            lifecycle->conversation_requested = false;
            aosl_atomic_set(&lifecycle->stop_request, MYBOT_STOP_REQUEST_NONE);
            action_start_conversation(lifecycle);
        } else {
            tick_runtime_binding_poll(lifecycle);
        }
        break;
    case MYBOT_DEVICE_STATE_IN_CONVERSATION:
        tick_conversation(lifecycle);
        break;
    }
}

void mybot_device_lifecycle_tick(mybot_device_lifecycle_t *lifecycle) {
    if (aosl_atomic_read(&lifecycle->shutting_down)) {
        return;
    }

    /* Consume a loss even if connectivity recovered before this tick. */
    bool lost = aosl_atomic_xchg(&lifecycle->network_loss_pending, false) != 0;
    if (lost || !aosl_atomic_read(&lifecycle->network_available)) {
        lifecycle->conversation_requested = false;
        aosl_atomic_set(&lifecycle->stop_request, MYBOT_STOP_REQUEST_NONE);
        if (lost && current_state(lifecycle) == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
            complete_conversation_locally(lifecycle);
        }
        return;
    }

    if (lifecycle->pairing_requested) {
        begin_pairing(lifecycle);
        return;
    }
    tick_device_state(lifecycle);
}

void mybot_device_lifecycle_set_network_available(mybot_device_lifecycle_t *lifecycle,
                                                  bool available) {
    bool was_available = aosl_atomic_xchg(&lifecycle->network_available, available) != 0;
    if (!available && was_available) {
        aosl_atomic_inc(&lifecycle->network_generation);
        aosl_atomic_set(&lifecycle->network_loss_pending, true);
        /* The control owner consumes this loss before accepting another command. */
    }
}

void mybot_device_lifecycle_shutdown(mybot_device_lifecycle_t *lifecycle) {
    aosl_atomic_set(&lifecycle->shutting_down, true);
    lifecycle->pairing_requested = false;
    lifecycle->conversation_requested = false;
    aosl_atomic_set(&lifecycle->stop_request, MYBOT_STOP_REQUEST_NONE);
    aosl_atomic_set(&lifecycle->rtc_token_renewal_requested, false);

    if (current_state(lifecycle) == MYBOT_DEVICE_STATE_IN_CONVERSATION &&
        aosl_atomic_read(&lifecycle->network_available) &&
        !aosl_atomic_read(&lifecycle->network_loss_pending)) {
        action_stop_conversation(lifecycle, MYBOT_CONVERSATION_STOP_REASON_DEVICE_HANGUP);
    } else if (current_state(lifecycle) == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        complete_conversation_locally(lifecycle);
    }
}

void mybot_device_lifecycle_request_pair(mybot_device_lifecycle_t *lifecycle) {
    if (aosl_atomic_read(&lifecycle->shutting_down)) {
        return;
    }
    lifecycle->pairing_requested = true;
}

void mybot_device_lifecycle_request_start(mybot_device_lifecycle_t *lifecycle) {
    if (aosl_atomic_read(&lifecycle->shutting_down) ||
        !aosl_atomic_read(&lifecycle->network_available)) {
        return;
    }
    if (current_state(lifecycle) != MYBOT_DEVICE_STATE_RUNTIME) {
        AOSL_LOG_ERR("cannot start: not in runtime");
        return;
    }
    lifecycle->conversation_requested = true;
}

void mybot_device_lifecycle_request_stop(mybot_device_lifecycle_t *lifecycle) {
    if (aosl_atomic_read(&lifecycle->shutting_down)) {
        return;
    }
    if (current_state(lifecycle) != MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        AOSL_LOG_ERR("cannot stop: not in conversation");
        return;
    }
    aosl_atomic_set(&lifecycle->stop_request, MYBOT_STOP_REQUEST_DEVICE_HANGUP);
}

void mybot_device_lifecycle_notify_conversation_ended(mybot_device_lifecycle_t *lifecycle) {
    if (aosl_atomic_read(&lifecycle->shutting_down)) {
        return;
    }
    /* RTC callbacks only publish this mailbox. The control owner leaves RTC,
     * then queues the service notification from tick(), avoiding RTC re-entry. */
    if (current_state(lifecycle) == MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        aosl_atomic_set(&lifecycle->stop_request, MYBOT_STOP_REQUEST_ERROR);
    }
}

void mybot_device_lifecycle_request_rtc_token_renewal(mybot_device_lifecycle_t *lifecycle) {
    if (aosl_atomic_read(&lifecycle->shutting_down) ||
        !aosl_atomic_read(&lifecycle->network_available)) {
        return;
    }
    if (current_state(lifecycle) != MYBOT_DEVICE_STATE_IN_CONVERSATION) {
        AOSL_LOG_WRN("ignoring RTC-token renewal request without an active conversation");
        return;
    }
    aosl_atomic_set(&lifecycle->rtc_token_renewal_requested, true);
}
