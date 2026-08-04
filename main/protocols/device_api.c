#include "device_api.h"
#include "mybot_config.h"
#include "http_client.h"
#include "cJSON.h"

#include <hal/aosl_hal_memory.h>
#include <api/aosl_log.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* ----------------------------------------------------------
 * JSON helpers — implemented via mybot_cJSON
 * ---------------------------------------------------------- */

char *mybot_device_api_json_build(const char *first_key, ...)
{
    mybot_cJSON *obj = mybot_cJSON_CreateObject();
    if (!obj) { return NULL; }

    va_list args;
    va_start(args, first_key);
    const char *key = first_key;

    while (key) {
        const char *val = va_arg(args, const char *);
        if (!val) { break; }
        mybot_cJSON_AddStringToObject(obj, key, val);
        key = va_arg(args, const char *);
    }
    va_end(args);

    char *json_str = mybot_cJSON_PrintUnformatted(obj);
    mybot_cJSON_Delete(obj);

    /* mybot_cJSON uses malloc/free; copy to aosl_hal memory for API contract */
    if (!json_str) { return NULL; }
    size_t len = strlen(json_str) + 1;
    char *result = (char *)aosl_hal_malloc(len);
    if (result) { memcpy(result, json_str, len); }
    free(json_str);
    return result;
}

char *mybot_device_api_json_get_string(const char *json, const char *key)
{
    if (!json || !key) { return NULL; }

    mybot_cJSON *root = mybot_cJSON_Parse(json);
    if (!root) { return NULL; }

    mybot_cJSON *item = mybot_cJSON_GetObjectItem(root, key);
    char *result = NULL;

    if (item && item->valuestring) {
        result = (char *)aosl_hal_malloc(strlen(item->valuestring) + 1);
        if (result) { strcpy(result, item->valuestring); }
    }

    mybot_cJSON_Delete(root);
    return result;
}

int mybot_device_api_json_get_int(const char *json, const char *key, int def)
{
    if (!json || !key) { return def; }

    mybot_cJSON *root = mybot_cJSON_Parse(json);
    if (!root) { return def; }

    mybot_cJSON *item = mybot_cJSON_GetObjectItem(root, key);
    int val = def;
    if (item) { val = (int)item->valueint; }

    mybot_cJSON_Delete(root);
    return val;
}

void mybot_device_api_json_free(void *ptr)
{
    aosl_hal_free(ptr);
}

/* ----------------------------------------------------------
 * Internal: extract nested RTC block from conversation start response
 * ---------------------------------------------------------- */
static int parse_rtc_block(mybot_cJSON *root, mybot_device_conversation_t *resp)
{
    mybot_cJSON *rtc = mybot_cJSON_GetObjectItem(root, "rtc");
    if (!rtc) { return -1; }

    mybot_cJSON *item;

    item = mybot_cJSON_GetObjectItem(rtc, "app_id");
    if (item && item->valuestring) {
        strncpy(resp->rtc_app_id, item->valuestring, sizeof(resp->rtc_app_id) - 1);
    }

    item = mybot_cJSON_GetObjectItem(rtc, "channel");
    if (item && item->valuestring) {
        strncpy(resp->rtc_channel, item->valuestring, sizeof(resp->rtc_channel) - 1);
    }

    item = mybot_cJSON_GetObjectItem(rtc, "token");
    if (item && item->valuestring) {
        strncpy(resp->rtc_token, item->valuestring, sizeof(resp->rtc_token) - 1);
    }

    item = mybot_cJSON_GetObjectItem(rtc, "uid");
    if (item && item->valuestring) {
        strncpy(resp->rtc_uid, item->valuestring, sizeof(resp->rtc_uid) - 1);
    }

    return 0;
}

/* ----------------------------------------------------------
 * Device API — server communication
 * ---------------------------------------------------------- */

int mybot_device_api_create_pair_code(const char *base_url,
                                      const char *device_id,
                                      const char *firmware_ver,
                                      const char *hw_model,
                                      mybot_device_pair_code_t *resp)
{
    if (!base_url || !device_id || !resp) {
        return -1;
    }
    memset(resp, 0, sizeof(*resp));

    /* Build request body */
    mybot_cJSON *body_obj = mybot_cJSON_CreateObject();
    mybot_cJSON_AddStringToObject(body_obj, "device_id", device_id);
    if (firmware_ver && firmware_ver[0]) {
        mybot_cJSON_AddStringToObject(body_obj, "firmware_version", firmware_ver);
    }
    if (hw_model && hw_model[0]) {
        mybot_cJSON_AddStringToObject(body_obj, "hardware_model", hw_model);
    }

    char *body = mybot_cJSON_PrintUnformatted(body_obj);
    mybot_cJSON_Delete(body_obj);
    if (!body) { return -1; }

    char url[MYBOT_DEVICE_API_MAX_URL];
    snprintf(url, sizeof(url), "%s/devices/pair-codes", base_url);

    AOSL_LOG_INF("POST %s body: %s", url, body);

    mybot_http_response_t raw;
    memset(&raw, 0, sizeof(raw));

    if (mybot_http_post(url, "application/json", body, &raw) < 0) {
        free(body);
        AOSL_LOG_ERR("POST %s failed (http)", url);
        return -1;
    }
    free(body);

    AOSL_LOG_INF("POST %s -> status=%d, body: %s",
                 url, raw.status_code,
                 raw.body ? raw.body : "(empty)");

    /* Parse with mybot_cJSON */
    mybot_cJSON *root = raw.body ? mybot_cJSON_Parse(raw.body) : NULL;
    if (!root) { mybot_http_response_free(&raw); return -1; }

    mybot_cJSON *data = mybot_cJSON_GetObjectItem(root, "data");
    if (!data) { mybot_cJSON_Delete(root); mybot_http_response_free(&raw); return -1; }

    mybot_cJSON *item;

    item = mybot_cJSON_GetObjectItem(data, "device_id");
    if (item && item->valuestring) {
        strncpy(resp->device_id, item->valuestring, sizeof(resp->device_id) - 1);
    }

    item = mybot_cJSON_GetObjectItem(data, "code");
    if (item && item->valuestring) {
        strncpy(resp->code, item->valuestring, sizeof(resp->code) - 1);
    }

    item = mybot_cJSON_GetObjectItem(data, "pair_token");
    if (item && item->valuestring) {
        strncpy(resp->pair_token, item->valuestring, sizeof(resp->pair_token) - 1);
    }

    item = mybot_cJSON_GetObjectItem(data, "expires_in_seconds");
    if (item) { resp->expires_in_seconds = (int)item->valueint; }

    item = mybot_cJSON_GetObjectItem(data, "poll_after_seconds");
    if (item) { resp->poll_after_seconds = (int)item->valueint; }

    AOSL_LOG_INF("pair_code: device_id=%s code=%s expires_in=%ds poll=%ds",
                 resp->device_id, resp->code,
                 resp->expires_in_seconds, resp->poll_after_seconds);

    mybot_cJSON_Delete(root);
    mybot_http_response_free(&raw);
    return 0;
}

int mybot_device_api_get_binding_status(const char *base_url,
                                        const char *device_id,
                                        const char *auth_header,
                                        mybot_device_binding_t *resp)
{
    if (!base_url || !device_id || !auth_header || !resp) {
        return -1;
    }
    memset(resp, 0, sizeof(*resp));

    char url[MYBOT_DEVICE_API_MAX_URL];
    snprintf(url, sizeof(url), "%s/devices/%s/binding-status",
             base_url, device_id);

    char extra_hdrs[MYBOT_DEVICE_API_MAX_TOKEN + 32];
    snprintf(extra_hdrs, sizeof(extra_hdrs), "Authorization: %s\r\n", auth_header);

    AOSL_LOG_INF("GET %s (auth=%s...)", url, auth_header);

    mybot_http_response_t raw;
    memset(&raw, 0, sizeof(raw));

    if (mybot_http_get_ex(url, extra_hdrs, &raw) < 0) {
        AOSL_LOG_ERR("GET %s failed (http)", url);
        return -1;
    }

    AOSL_LOG_INF("GET %s -> status=%d, body: %s",
                 url, raw.status_code,
                 raw.body ? raw.body : "(empty)");

    /* Parse with mybot_cJSON */
    mybot_cJSON *root = raw.body ? mybot_cJSON_Parse(raw.body) : NULL;
    if (!root) { mybot_http_response_free(&raw); return -1; }

    mybot_cJSON *data = mybot_cJSON_GetObjectItem(root, "data");
    if (!data) { mybot_cJSON_Delete(root); mybot_http_response_free(&raw); return -1; }

    mybot_cJSON *item;

    item = mybot_cJSON_GetObjectItem(data, "status");
    if (item && item->valuestring) {
        strncpy(resp->status, item->valuestring, sizeof(resp->status) - 1);
    }

    item = mybot_cJSON_GetObjectItem(data, "device_token");
    if (item && item->valuestring) {
        strncpy(resp->device_token, item->valuestring, sizeof(resp->device_token) - 1);
    }

    item = mybot_cJSON_GetObjectItem(data, "agent_id");
    if (item && item->valuestring) {
        strncpy(resp->agent_id, item->valuestring, sizeof(resp->agent_id) - 1);
    }

    item = mybot_cJSON_GetObjectItem(data, "agent_name");
    if (item && item->valuestring) {
        strncpy(resp->agent_name, item->valuestring, sizeof(resp->agent_name) - 1);
    }

    item = mybot_cJSON_GetObjectItem(data, "poll_after_seconds");
    if (item) { resp->poll_after_seconds = (int)item->valueint; }

    AOSL_LOG_DBG("binding: status=%s agent=%s has_token=%d poll=%ds",
                 resp->status, resp->agent_name,
                 resp->device_token[0] ? 1 : 0, resp->poll_after_seconds);

    mybot_cJSON_Delete(root);
    mybot_http_response_free(&raw);
    return 0;
}

int mybot_device_api_start_conversation(const char *base_url,
                                        const char *device_id,
                                        const char *device_token,
                                        const char *body_params,
                                        mybot_device_conversation_t *resp)
{
    if (!base_url || !device_id || !device_token || !resp) {
        return -1;
    }
    memset(resp, 0, sizeof(*resp));

    char url[MYBOT_DEVICE_API_MAX_URL];
    snprintf(url, sizeof(url), "%s/devices/%s/conversations/start",
             base_url, device_id);

    /* Build body: use caller-provided or construct from config macros */
    char *body = NULL;
    bool body_allocated = false;

    if (body_params) {
        body = (char *)body_params;
    } else {
        mybot_cJSON *root = mybot_cJSON_CreateObject();
        mybot_cJSON_AddStringToObject(root, "trigger", "button");

        mybot_cJSON *audio = mybot_cJSON_CreateObject();
        mybot_cJSON_AddNumberToObject(audio, "p_time", 20);
        mybot_cJSON_AddStringToObject(audio, "codec", "G722");
        mybot_cJSON_AddItemToObject(root, "audio", audio);

        mybot_cJSON *features = mybot_cJSON_CreateObject();
#if MYBOT_CLOUD_AEC
        mybot_cJSON_AddBoolToObject(features, "cloud_aec", 1);
#else
        mybot_cJSON_AddBoolToObject(features, "cloud_aec", 0);
#endif
#if MYBOT_AI_QOS
        mybot_cJSON_AddBoolToObject(features, "ai_qos", 1);
        mybot_cJSON_AddNumberToObject(features, "fast_send_multiplier", MYBOT_FAST_SEND_MULTIPLIER);
#else
        mybot_cJSON_AddBoolToObject(features, "ai_qos", 0);
#endif
        mybot_cJSON_AddBoolToObject(features, "show_transcript", MYBOT_SHOW_TRANSCRIPT);
        mybot_cJSON_AddItemToObject(root, "features", features);

        body = mybot_cJSON_PrintUnformatted(root);
        mybot_cJSON_Delete(root);
        body_allocated = true;
        if (!body) { AOSL_LOG_ERR("failed to build request body"); return -1; }
    }

    char extra_hdrs[MYBOT_DEVICE_API_MAX_TOKEN + 32];
    snprintf(extra_hdrs, sizeof(extra_hdrs), "Authorization: Device %s\r\n", device_token);

    AOSL_LOG_INF("POST %s body: %s", url, body);

    mybot_http_response_t raw;
    memset(&raw, 0, sizeof(raw));

    if (mybot_http_post_ex(url, "application/json", body, extra_hdrs, &raw) < 0) {
        AOSL_LOG_ERR("POST %s failed (http)", url);
        if (body_allocated) { free(body); }
        return -1;
    }
    if (body_allocated) { free(body); }

    AOSL_LOG_INF("POST %s -> status=%d, body: %s",
                 url, raw.status_code,
                 raw.body ? raw.body : "(empty)");

    /* Parse with mybot_cJSON */
    mybot_cJSON *root = raw.body ? mybot_cJSON_Parse(raw.body) : NULL;
    if (!root) { mybot_http_response_free(&raw); return -1; }

    mybot_cJSON *data = mybot_cJSON_GetObjectItem(root, "data");
    if (!data) { mybot_cJSON_Delete(root); mybot_http_response_free(&raw); return -1; }

    mybot_cJSON *item;

    item = mybot_cJSON_GetObjectItem(data, "conversation_id");
    if (item && item->valuestring) {
        strncpy(resp->conversation_id, item->valuestring, sizeof(resp->conversation_id) - 1);
    }

    /* Parse nested "rtc":{...} block */
    parse_rtc_block(data, resp);

    AOSL_LOG_INF("conversation: id=%s channel=%s uid=%s",
                 resp->conversation_id, resp->rtc_channel, resp->rtc_uid);

    mybot_cJSON_Delete(root);
    mybot_http_response_free(&raw);
    return 0;
}

int mybot_device_api_stop_conversation(const char *base_url,
                                       const char *device_id,
                                       const char *device_token,
                                       const char *conversation_id,
                                       const char *reason)
{
    if (!base_url || !device_id || !device_token || !conversation_id) {
        return -1;
    }

    char url[MYBOT_DEVICE_API_MAX_URL];
    snprintf(url, sizeof(url), "%s/devices/%s/conversations/stop",
             base_url, device_id);

    /* Build body with mybot_cJSON */
    mybot_cJSON *body_obj = mybot_cJSON_CreateObject();
    mybot_cJSON_AddStringToObject(body_obj, "conversation_id", conversation_id);
    if (reason) {
        mybot_cJSON_AddStringToObject(body_obj, "reason", reason);
    }

    char *body = mybot_cJSON_PrintUnformatted(body_obj);
    mybot_cJSON_Delete(body_obj);
    if (!body) { return -1; }

    char extra_hdrs[MYBOT_DEVICE_API_MAX_TOKEN + 32];
    snprintf(extra_hdrs, sizeof(extra_hdrs), "Authorization: Device %s\r\n", device_token);

    AOSL_LOG_INF("POST %s body: %s", url, body);

    mybot_http_response_t raw;
    memset(&raw, 0, sizeof(raw));

    int ret = mybot_http_post_ex(url, "application/json", body, extra_hdrs, &raw);
    free(body);

    if (ret == 0) {
        AOSL_LOG_INF("POST %s -> status=%d, body: %s",
                     url, raw.status_code,
                     raw.body ? raw.body : "(empty)");
    } else {
        AOSL_LOG_ERR("POST %s failed (http)", url);
    }

    mybot_http_response_free(&raw);
    return ret;
}
