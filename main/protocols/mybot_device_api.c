#include "mybot_device_api.h"
#include "mybot_config.h"
#include "mybot_utils_http_client.h"
#include "mybot_utils_cJSON.h"

#include <api/aosl_log.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ----------------------------------------------------------
 * Internal: extract nested RTC block from conversation start response
 * ---------------------------------------------------------- */
static int parse_rtc_block(mybot_utils_cJSON *root, mybot_device_conversation_t *resp) {
    mybot_utils_cJSON *rtc = mybot_utils_cJSON_GetObjectItem(root, "rtc");
    if (!rtc) {
        return -1;
    }

    mybot_utils_cJSON *item;

    item = mybot_utils_cJSON_GetObjectItem(rtc, "app_id");
    if (item && item->valuestring) {
        strncpy(resp->rtc_app_id, item->valuestring, sizeof(resp->rtc_app_id) - 1);
    }

    item = mybot_utils_cJSON_GetObjectItem(rtc, "channel");
    if (item && item->valuestring) {
        strncpy(resp->rtc_channel, item->valuestring, sizeof(resp->rtc_channel) - 1);
    }

    item = mybot_utils_cJSON_GetObjectItem(rtc, "token");
    if (item && item->valuestring) {
        strncpy(resp->rtc_token, item->valuestring, sizeof(resp->rtc_token) - 1);
    }

    item = mybot_utils_cJSON_GetObjectItem(rtc, "uid");
    if (item && item->valuestring) {
        strncpy(resp->rtc_uid, item->valuestring, sizeof(resp->rtc_uid) - 1);
    }

    /* Channel and UID are required to join RTC — without them the response is
     * unusable. Token may legitimately be absent (no-auth channel). */
    if (resp->rtc_channel[0] == '\0' || resp->rtc_uid[0] == '\0') {
        return -1;
    }

    return 0;
}

/* ----------------------------------------------------------
 * Device API — server communication
 * ---------------------------------------------------------- */

/* True if the HTTP response status is a 2xx success. */
static bool http_response_ok(const mybot_utils_http_response_t *resp) {
    return resp->status_code >= 200 && resp->status_code < 300;
}

int mybot_device_api_create_pair_code(const char *base_url, const char *device_id,
                                      const char *firmware_ver, const char *hw_model,
                                      mybot_device_pair_code_t *resp) {
    if (!base_url || !device_id || !resp) {
        return -1;
    }
    memset(resp, 0, sizeof(*resp));

    /* Build request body */
    mybot_utils_cJSON *body_obj = mybot_utils_cJSON_CreateObject();
    mybot_utils_cJSON_AddStringToObject(body_obj, "device_id", device_id);
    if (firmware_ver && firmware_ver[0]) {
        mybot_utils_cJSON_AddStringToObject(body_obj, "firmware_version", firmware_ver);
    }
    if (hw_model && hw_model[0]) {
        mybot_utils_cJSON_AddStringToObject(body_obj, "hardware_model", hw_model);
    }

    char *body = mybot_utils_cJSON_PrintUnformatted(body_obj);
    mybot_utils_cJSON_Delete(body_obj);
    if (!body) {
        return -1;
    }

    char url[MYBOT_DEVICE_API_MAX_URL];
    snprintf(url, sizeof(url), "%s/devices/pair-codes", base_url);

    AOSL_LOG_INF("POST %s body: %s", url, body);

    mybot_utils_http_response_t raw;
    memset(&raw, 0, sizeof(raw));

    if (mybot_utils_http_post(url, "application/json", body, &raw) < 0) {
        free(body);
        AOSL_LOG_ERR("POST %s failed (http)", url);
        return -1;
    }
    free(body);

    AOSL_LOG_INF("POST %s -> status=%d, body: %s", url, raw.status_code,
                 raw.body ? raw.body : "(empty)");

    if (!http_response_ok(&raw)) {
        int status = raw.status_code;
        AOSL_LOG_ERR("POST %s -> HTTP error %d", url, raw.status_code);
        mybot_utils_http_response_free(&raw);
        return status > 0 ? status : -1;
    }

    /* Parse with mybot_utils_cJSON */
    mybot_utils_cJSON *root = raw.body ? mybot_utils_cJSON_Parse(raw.body) : NULL;
    if (!root) {
        mybot_utils_http_response_free(&raw);
        return -1;
    }

    mybot_utils_cJSON *data = mybot_utils_cJSON_GetObjectItem(root, "data");
    if (!data) {
        mybot_utils_cJSON_Delete(root);
        mybot_utils_http_response_free(&raw);
        return -1;
    }

    mybot_utils_cJSON *item;

    item = mybot_utils_cJSON_GetObjectItem(data, "device_id");
    if (item && item->valuestring) {
        strncpy(resp->device_id, item->valuestring, sizeof(resp->device_id) - 1);
    }

    item = mybot_utils_cJSON_GetObjectItem(data, "code");
    if (item && item->valuestring) {
        strncpy(resp->code, item->valuestring, sizeof(resp->code) - 1);
    }

    item = mybot_utils_cJSON_GetObjectItem(data, "pair_token");
    if (item && item->valuestring) {
        strncpy(resp->pair_token, item->valuestring, sizeof(resp->pair_token) - 1);
    }

    item = mybot_utils_cJSON_GetObjectItem(data, "expires_in_seconds");
    if (item) {
        resp->expires_in_seconds = (int)item->valueint;
    }

    item = mybot_utils_cJSON_GetObjectItem(data, "poll_after_seconds");
    if (item) {
        resp->poll_after_seconds = (int)item->valueint;
    }

    AOSL_LOG_INF("pair_code: device_id=%s code=%s expires_in=%ds poll=%ds", resp->device_id,
                 resp->code, resp->expires_in_seconds, resp->poll_after_seconds);

    mybot_utils_cJSON_Delete(root);
    mybot_utils_http_response_free(&raw);
    return 0;
}

int mybot_device_api_get_binding_status(const char *base_url, const char *device_id,
                                        const char *auth_header, mybot_device_binding_t *resp) {
    if (!base_url || !device_id || !auth_header || !resp) {
        return -1;
    }
    memset(resp, 0, sizeof(*resp));

    char url[MYBOT_DEVICE_API_MAX_URL];
    snprintf(url, sizeof(url), "%s/devices/%s/binding-status", base_url, device_id);

    char extra_hdrs[MYBOT_DEVICE_API_MAX_TOKEN + 32];
    snprintf(extra_hdrs, sizeof(extra_hdrs), "Authorization: %s\r\n", auth_header);

    AOSL_LOG_INF("GET %s (auth=%s...)", url, auth_header);

    mybot_utils_http_response_t raw;
    memset(&raw, 0, sizeof(raw));

    if (mybot_utils_http_get_ex(url, extra_hdrs, &raw) < 0) {
        AOSL_LOG_ERR("GET %s failed (http)", url);
        return -1;
    }

    AOSL_LOG_INF("GET %s -> status=%d, body: %s", url, raw.status_code,
                 raw.body ? raw.body : "(empty)");

    if (!http_response_ok(&raw)) {
        int status = raw.status_code;
        AOSL_LOG_ERR("GET %s -> HTTP error %d", url, raw.status_code);
        mybot_utils_http_response_free(&raw);
        return status > 0 ? status : -1;
    }

    /* Parse with mybot_utils_cJSON */
    mybot_utils_cJSON *root = raw.body ? mybot_utils_cJSON_Parse(raw.body) : NULL;
    if (!root) {
        mybot_utils_http_response_free(&raw);
        return -1;
    }

    mybot_utils_cJSON *data = mybot_utils_cJSON_GetObjectItem(root, "data");
    if (!data) {
        mybot_utils_cJSON_Delete(root);
        mybot_utils_http_response_free(&raw);
        return -1;
    }

    mybot_utils_cJSON *item;

    item = mybot_utils_cJSON_GetObjectItem(data, "status");
    if (item && item->valuestring) {
        strncpy(resp->status, item->valuestring, sizeof(resp->status) - 1);
    }

    item = mybot_utils_cJSON_GetObjectItem(data, "device_token");
    if (item && item->valuestring) {
        strncpy(resp->device_token, item->valuestring, sizeof(resp->device_token) - 1);
    }

    item = mybot_utils_cJSON_GetObjectItem(data, "agent_id");
    if (item && item->valuestring) {
        strncpy(resp->agent_id, item->valuestring, sizeof(resp->agent_id) - 1);
    }

    item = mybot_utils_cJSON_GetObjectItem(data, "agent_name");
    if (item && item->valuestring) {
        strncpy(resp->agent_name, item->valuestring, sizeof(resp->agent_name) - 1);
    }

    item = mybot_utils_cJSON_GetObjectItem(data, "poll_after_seconds");
    if (item) {
        resp->poll_after_seconds = (int)item->valueint;
    }

    AOSL_LOG_DBG("binding: status=%s agent=%s has_token=%d poll=%ds", resp->status,
                 resp->agent_name, resp->device_token[0] ? 1 : 0, resp->poll_after_seconds);

    mybot_utils_cJSON_Delete(root);
    mybot_utils_http_response_free(&raw);
    return 0;
}

int mybot_device_api_start_conversation(const char *base_url, const char *device_id,
                                        const char *device_token, const char *body_params,
                                        mybot_device_conversation_t *resp) {
    if (!base_url || !device_id || !device_token || !resp) {
        return -1;
    }
    memset(resp, 0, sizeof(*resp));

    char url[MYBOT_DEVICE_API_MAX_URL];
    snprintf(url, sizeof(url), "%s/devices/%s/conversations/start", base_url, device_id);

    /* Build body: use caller-provided or construct from config macros */
    char *body = NULL;
    bool body_allocated = false;

    if (body_params) {
        body = (char *)body_params;
    } else {
        mybot_utils_cJSON *root = mybot_utils_cJSON_CreateObject();
        mybot_utils_cJSON_AddStringToObject(root, "trigger", "button");

        mybot_utils_cJSON *audio = mybot_utils_cJSON_CreateObject();
        mybot_utils_cJSON_AddNumberToObject(audio, "p_time", 20);
        mybot_utils_cJSON_AddStringToObject(audio, "codec", "G722");
        mybot_utils_cJSON_AddItemToObject(root, "audio", audio);

        mybot_utils_cJSON *features = mybot_utils_cJSON_CreateObject();
#if MYBOT_CLOUD_AEC
        mybot_utils_cJSON_AddBoolToObject(features, "cloud_aec", 1);
#else
        mybot_utils_cJSON_AddBoolToObject(features, "cloud_aec", 0);
#endif
#if MYBOT_AI_QOS
        mybot_utils_cJSON_AddBoolToObject(features, "ai_qos", 1);
        mybot_utils_cJSON_AddNumberToObject(features, "fast_send_multiplier",
                                            MYBOT_FAST_SEND_MULTIPLIER);
#else
        mybot_utils_cJSON_AddBoolToObject(features, "ai_qos", 0);
#endif
        mybot_utils_cJSON_AddBoolToObject(features, "show_transcript", MYBOT_SHOW_TRANSCRIPT);
        mybot_utils_cJSON_AddItemToObject(root, "features", features);

        body = mybot_utils_cJSON_PrintUnformatted(root);
        mybot_utils_cJSON_Delete(root);
        body_allocated = true;
        if (!body) {
            AOSL_LOG_ERR("failed to build request body");
            return -1;
        }
    }

    char extra_hdrs[MYBOT_DEVICE_API_MAX_TOKEN + 32];
    snprintf(extra_hdrs, sizeof(extra_hdrs), "Authorization: Device %s\r\n", device_token);

    AOSL_LOG_INF("POST %s body: %s", url, body);

    mybot_utils_http_response_t raw;
    memset(&raw, 0, sizeof(raw));

    if (mybot_utils_http_post_ex(url, "application/json", body, extra_hdrs, &raw) < 0) {
        AOSL_LOG_ERR("POST %s failed (http)", url);
        if (body_allocated) {
            free(body);
        }
        return -1;
    }
    if (body_allocated) {
        free(body);
    }

    AOSL_LOG_INF("POST %s -> status=%d, body: %s", url, raw.status_code,
                 raw.body ? raw.body : "(empty)");

    if (!http_response_ok(&raw)) {
        int status = raw.status_code;
        AOSL_LOG_ERR("POST %s -> HTTP error %d", url, raw.status_code);
        mybot_utils_http_response_free(&raw);
        return status > 0 ? status : -1;
    }

    /* Parse with mybot_utils_cJSON */
    mybot_utils_cJSON *root = raw.body ? mybot_utils_cJSON_Parse(raw.body) : NULL;
    if (!root) {
        mybot_utils_http_response_free(&raw);
        return -1;
    }

    mybot_utils_cJSON *data = mybot_utils_cJSON_GetObjectItem(root, "data");
    if (!data) {
        mybot_utils_cJSON_Delete(root);
        mybot_utils_http_response_free(&raw);
        return -1;
    }

    mybot_utils_cJSON *item;

    item = mybot_utils_cJSON_GetObjectItem(data, "conversation_id");
    if (item && item->valuestring) {
        strncpy(resp->conversation_id, item->valuestring, sizeof(resp->conversation_id) - 1);
    }

    /* Parse nested "rtc":{...} block — required to join RTC. */
    if (parse_rtc_block(data, resp) < 0) {
        AOSL_LOG_ERR("conversation response missing rtc block");
        mybot_utils_cJSON_Delete(root);
        mybot_utils_http_response_free(&raw);
        return -1;
    }

    AOSL_LOG_INF("conversation: id=%s channel=%s uid=%s", resp->conversation_id, resp->rtc_channel,
                 resp->rtc_uid);

    mybot_utils_cJSON_Delete(root);
    mybot_utils_http_response_free(&raw);
    return 0;
}

int mybot_device_api_stop_conversation(const char *base_url, const char *device_id,
                                       const char *device_token, const char *conversation_id,
                                       const char *reason) {
    if (!base_url || !device_id || !device_token || !conversation_id) {
        return -1;
    }

    char url[MYBOT_DEVICE_API_MAX_URL];
    snprintf(url, sizeof(url), "%s/devices/%s/conversations/stop", base_url, device_id);

    /* Build body with mybot_utils_cJSON */
    mybot_utils_cJSON *body_obj = mybot_utils_cJSON_CreateObject();
    mybot_utils_cJSON_AddStringToObject(body_obj, "conversation_id", conversation_id);
    if (reason) {
        mybot_utils_cJSON_AddStringToObject(body_obj, "reason", reason);
    }

    char *body = mybot_utils_cJSON_PrintUnformatted(body_obj);
    mybot_utils_cJSON_Delete(body_obj);
    if (!body) {
        return -1;
    }

    char extra_hdrs[MYBOT_DEVICE_API_MAX_TOKEN + 32];
    snprintf(extra_hdrs, sizeof(extra_hdrs), "Authorization: Device %s\r\n", device_token);

    AOSL_LOG_INF("POST %s body: %s", url, body);

    mybot_utils_http_response_t raw;
    memset(&raw, 0, sizeof(raw));

    int ret = mybot_utils_http_post_ex(url, "application/json", body, extra_hdrs, &raw);
    free(body);

    if (ret == 0) {
        AOSL_LOG_INF("POST %s -> status=%d, body: %s", url, raw.status_code,
                     raw.body ? raw.body : "(empty)");
        if (!http_response_ok(&raw)) {
            AOSL_LOG_ERR("POST %s -> HTTP error %d", url, raw.status_code);
            ret = raw.status_code > 0 ? raw.status_code : -1;
        }
    } else {
        AOSL_LOG_ERR("POST %s failed (http)", url);
    }

    mybot_utils_http_response_free(&raw);
    return ret;
}
