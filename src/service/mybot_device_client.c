/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_device_client.h"
#include <mybot/mybot_build_config.h>
#include "mybot_http_client.h"
#include "mybot_json.h"

#include <api/aosl_log.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>

static bool header_value_is_safe(const char *value) {
    if (!value || !value[0]) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) {
            return false;
        }
    }
    return true;
}

static bool url_unreserved(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '.' || c == '_' || c == '~';
}

static int encode_path_segment(const char *value, char *encoded, size_t encoded_size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t offset = 0;
    if (!value || !value[0] || !encoded || encoded_size == 0) {
        return -1;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        size_t needed = url_unreserved(*p) ? 1U : 3U;
        if (needed >= encoded_size - offset) {
            return -1;
        }
        if (needed == 1) {
            encoded[offset++] = (char)*p;
        } else {
            encoded[offset++] = '%';
            encoded[offset++] = hex[*p >> 4];
            encoded[offset++] = hex[*p & 0x0f];
        }
    }
    encoded[offset] = '\0';
    return 0;
}

static int build_device_url(const char *base_url, const char *device_id, const char *suffix,
                            char *url, size_t url_size) {
    char encoded_id[MYBOT_DEVICE_CLIENT_MAX_ID * 3];
    if (!base_url || !suffix ||
        encode_path_segment(device_id, encoded_id, sizeof(encoded_id)) < 0) {
        return -1;
    }
    int written = snprintf(url, url_size, "%s/devices/%s%s", base_url, encoded_id, suffix);
    return written >= 0 && (size_t)written < url_size ? 0 : -1;
}

static int build_authorization_header(const char *scheme, const char *credential, char *header,
                                      size_t header_size) {
    if (!scheme || !header_value_is_safe(credential)) {
        return -1;
    }
    int written = snprintf(header, header_size, "Authorization: %s%s\r\n", scheme, credential);
    return written >= 0 && (size_t)written < header_size ? 0 : -1;
}

static void copy_json_string(const mybot_json_t *object, const char *name, char *destination,
                             size_t destination_size) {
    const char *value = mybot_json_get_string(mybot_json_get_object_item(object, name));
    if (value && destination_size > 0) {
        snprintf(destination, destination_size, "%s", value);
    }
}

static int copy_required_json_string(const mybot_json_t *object, const char *name,
                                     char *destination, size_t destination_size) {
    const char *value = mybot_json_get_string(mybot_json_get_object_item(object, name));
    if (!value || !value[0] || destination_size == 0) {
        return -1;
    }

    size_t len = strlen(value);
    if (len >= destination_size) {
        return -1;
    }
    memcpy(destination, value, len + 1);
    return 0;
}

static void copy_json_integer(const mybot_json_t *object, const char *name, int *destination) {
    int64_t value;
    if (mybot_json_get_integer(mybot_json_get_object_item(object, name), &value)) {
        if (value > INT_MAX) {
            *destination = INT_MAX;
        } else if (value < INT_MIN) {
            *destination = INT_MIN;
        } else {
            *destination = (int)value;
        }
    }
}

static char *build_pair_code_body(const char *device_id, const char *firmware_ver,
                                  const char *hw_model) {
    mybot_json_t *root = mybot_json_create_object();
    if (!root || mybot_json_add_string(root, "device_id", device_id) < 0 ||
        (firmware_ver && firmware_ver[0] &&
         mybot_json_add_string(root, "firmware_version", firmware_ver) < 0) ||
        (hw_model && hw_model[0] && mybot_json_add_string(root, "hardware_model", hw_model) < 0)) {
        mybot_json_delete(root);
        return NULL;
    }

    char *body = mybot_json_print_unformatted(root);
    mybot_json_delete(root);
    return body;
}

static char *build_conversation_start_body(void) {
    mybot_json_t *root = mybot_json_create_object();
    mybot_json_t *audio = NULL;
    mybot_json_t *features = NULL;
    char *body = NULL;

    if (!root || mybot_json_add_string(root, "trigger", "button") < 0) {
        goto done;
    }

    audio = mybot_json_create_object();
    if (!audio || mybot_json_add_number(audio, "p_time", MYBOT_AUDIO_PTIME_MS) < 0 ||
        mybot_json_add_string(audio, "codec", "G722") < 0) {
        goto done;
    }
    int add_result = mybot_json_add_item(root, "audio", audio);
    if (add_result < 0) {
        goto done;
    }
    audio = NULL;

    features = mybot_json_create_object();
    if (!features) {
        goto done;
    }
#if MYBOT_CLOUD_AEC
    if (mybot_json_add_bool(features, "cloud_aec", true) < 0) {
#else
    if (mybot_json_add_bool(features, "cloud_aec", false) < 0) {
#endif
        goto done;
    }
#if MYBOT_AI_QOS
    if (mybot_json_add_bool(features, "ai_qos", true) < 0 ||
        mybot_json_add_number(features, "fast_send_multiplier", MYBOT_FAST_SEND_MULTIPLIER) < 0) {
#else
    if (mybot_json_add_bool(features, "ai_qos", false) < 0) {
#endif
        goto done;
    }
    add_result = mybot_json_add_item(root, "features", features);
    if (add_result < 0) {
        goto done;
    }
    features = NULL;

    body = mybot_json_print_unformatted(root);

done:
    mybot_json_delete(features);
    mybot_json_delete(audio);
    mybot_json_delete(root);
    return body;
}

static char *build_stop_conversation_body(const char *conversation_id, const char *reason) {
    mybot_json_t *root = mybot_json_create_object();
    if (!root || mybot_json_add_string(root, "conversation_id", conversation_id) < 0 ||
        (reason && mybot_json_add_string(root, "reason", reason) < 0)) {
        mybot_json_delete(root);
        return NULL;
    }

    char *body = mybot_json_print_unformatted(root);
    mybot_json_delete(root);
    return body;
}

static char *build_rtc_token_body(const char *channel, const char *local_uid) {
    mybot_json_t *root = mybot_json_create_object();
    if (!root || mybot_json_add_string(root, "channel", channel) < 0 ||
        mybot_json_add_string(root, "local_uid", local_uid) < 0) {
        mybot_json_delete(root);
        return NULL;
    }

    char *body = mybot_json_print_unformatted(root);
    mybot_json_delete(root);
    return body;
}

/* ----------------------------------------------------------
 * Internal: extract nested RTC block from conversation start response
 * ---------------------------------------------------------- */
static int parse_rtc_block(mybot_json_t *root, mybot_device_conversation_t *resp) {
    mybot_json_t *rtc = mybot_json_get_object_item(root, "rtc");
    if (!rtc) {
        return -1;
    }

    copy_json_string(rtc, "app_id", resp->rtc_app_id, sizeof(resp->rtc_app_id));
    copy_json_string(rtc, "channel", resp->rtc_channel, sizeof(resp->rtc_channel));
    copy_json_string(rtc, "token", resp->rtc_token, sizeof(resp->rtc_token));
    copy_json_string(rtc, "uid", resp->rtc_uid, sizeof(resp->rtc_uid));

    /* Channel and UID are required to join RTC — without them the response is
     * unusable. Token may legitimately be absent (no-auth channel). */
    if (resp->rtc_channel[0] == '\0' || resp->rtc_uid[0] == '\0') {
        return -1;
    }

    return 0;
}

/* ----------------------------------------------------------
 * Device service HTTP client
 * ---------------------------------------------------------- */

/* True if the HTTP response status is a 2xx success. */
static bool http_response_ok(const mybot_http_client_response_t *resp) {
    return resp->status_code >= 200 && resp->status_code < 300;
}

int mybot_device_client_create_pair_code(const char *base_url, const char *device_id,
                                         const char *firmware_ver, const char *hw_model,
                                         mybot_device_pair_code_t *resp) {
    if (!base_url || !device_id || !resp) {
        return -1;
    }
    memset(resp, 0, sizeof(*resp));

    char *body = build_pair_code_body(device_id, firmware_ver, hw_model);
    if (!body) {
        return -1;
    }

    char url[MYBOT_DEVICE_CLIENT_MAX_URL];
    snprintf(url, sizeof(url), "%s/devices/pair-codes", base_url);

    AOSL_LOG_NTC("POST %s", url);

    mybot_http_client_response_t raw;
    memset(&raw, 0, sizeof(raw));

    if (mybot_http_client_post_ex(url, "application/json", body, NULL, &raw) < 0) {
        mybot_json_free_string(body);
        AOSL_LOG_ERR("POST %s failed (http)", url);
        return -1;
    }
    mybot_json_free_string(body);

    AOSL_LOG_NTC("POST %s -> status=%d", url, raw.status_code);

    if (!http_response_ok(&raw)) {
        int status = raw.status_code;
        AOSL_LOG_ERR("POST %s -> HTTP error %d", url, raw.status_code);
        mybot_http_client_response_free(&raw);
        return status > 0 ? status : -1;
    }

    /* Parse with the namespaced JSON API. */
    mybot_json_t *root = raw.body ? mybot_json_parse(raw.body) : NULL;
    if (!root) {
        mybot_http_client_response_free(&raw);
        return -1;
    }

    mybot_json_t *data = mybot_json_get_object_item(root, "data");
    if (!data) {
        mybot_json_delete(root);
        mybot_http_client_response_free(&raw);
        return -1;
    }

    int code_result = copy_required_json_string(data, "code", resp->code, sizeof(resp->code));
    int pair_token_result =
        copy_required_json_string(data, "pair_token", resp->pair_token, sizeof(resp->pair_token));
    if (code_result < 0 || pair_token_result < 0) {
        AOSL_LOG_ERR("pair-code response missing or invalid code/pair_token");
        mybot_json_delete(root);
        mybot_http_client_response_free(&raw);
        return -1;
    }
    copy_json_integer(data, "poll_after_seconds", &resp->poll_after_seconds);

    AOSL_LOG_NTC("pair_code: code=%s poll=%ds", resp->code, resp->poll_after_seconds);

    mybot_json_delete(root);
    mybot_http_client_response_free(&raw);
    return 0;
}

int mybot_device_client_get_binding_status(const char *base_url, const char *device_id,
                                           const char *auth_header, mybot_device_binding_t *resp) {
    if (!base_url || !device_id || !auth_header || !resp) {
        return -1;
    }
    memset(resp, 0, sizeof(*resp));

    char url[MYBOT_DEVICE_CLIENT_MAX_URL];
    if (build_device_url(base_url, device_id, "/binding-status", url, sizeof(url)) < 0) {
        return -1;
    }

    char extra_hdrs[MYBOT_DEVICE_CLIENT_MAX_TOKEN + 32];
    if (build_authorization_header("", auth_header, extra_hdrs, sizeof(extra_hdrs)) < 0) {
        return -1;
    }

    AOSL_LOG_NTC("GET %s", url);

    mybot_http_client_response_t raw;
    memset(&raw, 0, sizeof(raw));

    if (mybot_http_client_get_ex(url, extra_hdrs, &raw) < 0) {
        AOSL_LOG_ERR("GET %s failed (http)", url);
        return -1;
    }

    AOSL_LOG_NTC("GET %s -> status=%d", url, raw.status_code);

    if (!http_response_ok(&raw)) {
        int status = raw.status_code;
        AOSL_LOG_ERR("GET %s -> HTTP error %d", url, raw.status_code);
        mybot_http_client_response_free(&raw);
        return status > 0 ? status : -1;
    }

    /* Parse with the namespaced JSON API. */
    mybot_json_t *root = raw.body ? mybot_json_parse(raw.body) : NULL;
    if (!root) {
        mybot_http_client_response_free(&raw);
        return -1;
    }

    mybot_json_t *data = mybot_json_get_object_item(root, "data");
    if (!data) {
        mybot_json_delete(root);
        mybot_http_client_response_free(&raw);
        return -1;
    }

    copy_json_string(data, "status", resp->status, sizeof(resp->status));
    copy_json_string(data, "device_token", resp->device_token, sizeof(resp->device_token));
    copy_json_string(data, "agent_id", resp->agent_id, sizeof(resp->agent_id));
    copy_json_string(data, "agent_name", resp->agent_name, sizeof(resp->agent_name));
    copy_json_integer(data, "poll_after_seconds", &resp->poll_after_seconds);

    AOSL_LOG_DBG("binding: status=%s agent=%s has_token=%d poll=%ds", resp->status,
                 resp->agent_name, resp->device_token[0] ? 1 : 0, resp->poll_after_seconds);

    mybot_json_delete(root);
    mybot_http_client_response_free(&raw);
    return 0;
}

int mybot_device_client_start_conversation(const char *base_url, const char *device_id,
                                           const char *device_token, const char *body_params,
                                           mybot_device_conversation_t *resp) {
    if (!base_url || !device_id || !device_token || !resp) {
        return -1;
    }
    memset(resp, 0, sizeof(*resp));

    char url[MYBOT_DEVICE_CLIENT_MAX_URL];
    if (build_device_url(base_url, device_id, "/conversations/start", url, sizeof(url)) < 0) {
        return -1;
    }

    /* Build body: use caller-provided or construct from config macros */
    char *generated_body = NULL;
    const char *body = body_params;

    if (!body) {
        generated_body = build_conversation_start_body();
        body = generated_body;
        if (!generated_body) {
            AOSL_LOG_ERR("failed to build request body");
            return -1;
        }
    }

    char extra_hdrs[MYBOT_DEVICE_CLIENT_MAX_TOKEN + 32];
    if (build_authorization_header("Device ", device_token, extra_hdrs, sizeof(extra_hdrs)) < 0) {
        mybot_json_free_string(generated_body);
        return -1;
    }

    AOSL_LOG_NTC("POST %s", url);

    mybot_http_client_response_t raw;
    memset(&raw, 0, sizeof(raw));

    if (mybot_http_client_post_ex(url, "application/json", body, extra_hdrs, &raw) < 0) {
        AOSL_LOG_ERR("POST %s failed (http)", url);
        mybot_json_free_string(generated_body);
        return -1;
    }
    mybot_json_free_string(generated_body);

    AOSL_LOG_NTC("POST %s -> status=%d", url, raw.status_code);

    if (!http_response_ok(&raw)) {
        int status = raw.status_code;
        AOSL_LOG_ERR("POST %s -> HTTP error %d", url, raw.status_code);
        mybot_http_client_response_free(&raw);
        return status > 0 ? status : -1;
    }

    /* Parse with the namespaced JSON API. */
    mybot_json_t *root = raw.body ? mybot_json_parse(raw.body) : NULL;
    if (!root) {
        mybot_http_client_response_free(&raw);
        return -1;
    }

    mybot_json_t *data = mybot_json_get_object_item(root, "data");
    if (!data) {
        mybot_json_delete(root);
        mybot_http_client_response_free(&raw);
        return -1;
    }

    if (copy_required_json_string(data, "conversation_id", resp->conversation_id,
                                  sizeof(resp->conversation_id)) < 0) {
        AOSL_LOG_ERR("conversation response missing or invalid conversation_id");
        mybot_json_delete(root);
        mybot_http_client_response_free(&raw);
        return -1;
    }

    /* Parse nested "rtc":{...} block — required to join RTC. */
    if (parse_rtc_block(data, resp) < 0) {
        AOSL_LOG_ERR("conversation response missing rtc block");
        mybot_json_delete(root);
        mybot_http_client_response_free(&raw);
        return -1;
    }

    AOSL_LOG_NTC("conversation: id=%s channel=%s uid=%s", resp->conversation_id, resp->rtc_channel,
                 resp->rtc_uid);

    mybot_json_delete(root);
    mybot_http_client_response_free(&raw);
    return 0;
}

int mybot_device_client_renew_rtc_token(const char *base_url, const char *device_id,
                                        const char *device_token, const char *channel,
                                        const char *local_uid, mybot_device_rtc_token_t *resp) {
    if (!base_url || !device_id || !device_token || !channel || !channel[0] || !local_uid ||
        !local_uid[0] || !resp) {
        return -1;
    }
    memset(resp, 0, sizeof(*resp));

    char url[MYBOT_DEVICE_CLIENT_MAX_URL];
    if (build_device_url(base_url, device_id, "/rtc-token", url, sizeof(url)) < 0) {
        return -1;
    }

    char *body = build_rtc_token_body(channel, local_uid);
    if (!body) {
        return -1;
    }

    char extra_hdrs[MYBOT_DEVICE_CLIENT_MAX_TOKEN + 32];
    if (build_authorization_header("Device ", device_token, extra_hdrs, sizeof(extra_hdrs)) < 0) {
        mybot_json_free_string(body);
        return -1;
    }

    AOSL_LOG_NTC("POST %s", url);

    mybot_http_client_response_t raw;
    memset(&raw, 0, sizeof(raw));
    int ret = mybot_http_client_post_ex(url, "application/json", body, extra_hdrs, &raw);
    mybot_json_free_string(body);
    if (ret < 0) {
        AOSL_LOG_ERR("POST %s failed (http)", url);
        return -1;
    }

    AOSL_LOG_NTC("POST %s -> status=%d", url, raw.status_code);
    if (!http_response_ok(&raw)) {
        int status = raw.status_code;
        AOSL_LOG_ERR("POST %s -> HTTP error %d", url, status);
        mybot_http_client_response_free(&raw);
        return status > 0 ? status : -1;
    }

    mybot_json_t *root = raw.body ? mybot_json_parse(raw.body) : NULL;
    if (!root) {
        mybot_http_client_response_free(&raw);
        return -1;
    }

    mybot_json_t *data = mybot_json_get_object_item(root, "data");
    mybot_json_t *rtc = data ? mybot_json_get_object_item(data, "rtc") : NULL;
    if (!rtc ||
        copy_required_json_string(rtc, "channel", resp->rtc_channel, sizeof(resp->rtc_channel)) <
            0 ||
        copy_required_json_string(rtc, "uid", resp->rtc_uid, sizeof(resp->rtc_uid)) < 0 ||
        copy_required_json_string(rtc, "token", resp->rtc_token, sizeof(resp->rtc_token)) < 0) {
        AOSL_LOG_ERR("RTC-token response missing required rtc fields");
        mybot_json_delete(root);
        mybot_http_client_response_free(&raw);
        return -1;
    }
    mybot_json_delete(root);
    mybot_http_client_response_free(&raw);
    return 0;
}

int mybot_device_client_stop_conversation(const char *base_url, const char *device_id,
                                          const char *device_token, const char *conversation_id,
                                          const char *reason) {
    if (!base_url || !device_id || !device_token || !conversation_id) {
        return -1;
    }

    char url[MYBOT_DEVICE_CLIENT_MAX_URL];
    if (build_device_url(base_url, device_id, "/conversations/stop", url, sizeof(url)) < 0) {
        return -1;
    }

    char *body = build_stop_conversation_body(conversation_id, reason);
    if (!body) {
        return -1;
    }

    char extra_hdrs[MYBOT_DEVICE_CLIENT_MAX_TOKEN + 32];
    if (build_authorization_header("Device ", device_token, extra_hdrs, sizeof(extra_hdrs)) < 0) {
        mybot_json_free_string(body);
        return -1;
    }

    AOSL_LOG_NTC("POST %s", url);

    mybot_http_client_response_t raw;
    memset(&raw, 0, sizeof(raw));

    int ret = mybot_http_client_post_ex(url, "application/json", body, extra_hdrs, &raw);
    mybot_json_free_string(body);

    if (ret == 0) {
        AOSL_LOG_NTC("POST %s -> status=%d", url, raw.status_code);
        if (!http_response_ok(&raw)) {
            AOSL_LOG_ERR("POST %s -> HTTP error %d", url, raw.status_code);
            ret = raw.status_code > 0 ? raw.status_code : -1;
        }
    } else {
        AOSL_LOG_ERR("POST %s failed (http)", url);
    }

    mybot_http_client_response_free(&raw);
    return ret;
}
