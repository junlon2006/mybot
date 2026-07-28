#include "device_api.h"
#include "http_client.h"

#include <hal/aosl_hal_memory.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* ----------------------------------------------------------
 * JSON helpers — minimal, embedded-oriented
 * ---------------------------------------------------------- */

char *device_api_json_build(const char *first_key, ...)
{
    size_t cap = 512;
    char *buf = (char *)aosl_hal_malloc(cap);
    if (!buf) return NULL;
    int pos = snprintf(buf, cap, "{");
    int first = 1;

    va_list args;
    va_start(args, first_key);
    const char *key = first_key;

    while (key) {
        const char *val = va_arg(args, const char *);
        if (!val) val = "";

        if (!first) {
            int n = snprintf(buf + pos, cap - (size_t)pos, ",");
            if (n > 0 && (size_t)n < cap - (size_t)pos) pos += n;
        }
        first = 0;

        int n = snprintf(buf + pos, cap - (size_t)pos,
                         "\"%s\":\"%s\"", key, val);
        if (n < 0) break;
        pos += n;
        key = va_arg(args, const char *);
    }
    va_end(args);

    if ((size_t)pos + 2 < cap) {
        buf[pos++] = '}';
        buf[pos] = '\0';
    }
    return buf;
}

char *device_api_json_get_string(const char *json, const char *key)
{
    if (!json || !key) return NULL;

    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);

    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);

    const char *end = strchr(p, '"');
    if (!end) return NULL;

    size_t len = (size_t)(end - p);
    char *val = (char *)aosl_hal_malloc(len + 1);
    if (!val) return NULL;
    memcpy(val, p, len);
    val[len] = '\0';
    return val;
}

int device_api_json_get_int(const char *json, const char *key, int def)
{
    if (!json || !key) return def;

    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);

    const char *p = strstr(json, pattern);
    if (!p) return def;
    p += strlen(pattern);

    while (*p == ' ') p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    int val = 0;
    while (*p >= '0' && *p <= '9')
        val = val * 10 + (*p++ - '0');
    return neg ? -val : val;
}

void device_api_json_free(void *ptr)
{
    aosl_hal_free(ptr);
}

/* ----------------------------------------------------------
 * Device API — server communication
 * ---------------------------------------------------------- */

int device_api_create_pair_code(const char *base_url,
                                const char *device_id,
                                const char *firmware_ver,
                                const char *hw_model,
                                device_pair_code_t *resp)
{
    if (!base_url || !device_id || !resp)
        return -1;
    memset(resp, 0, sizeof(*resp));

    /* Build request body */
    char body[512];
    int n = snprintf(body, sizeof(body),
        "{\"device_id\":\"%s\"", device_id);

    if (firmware_ver && firmware_ver[0])
        n += snprintf(body + n, sizeof(body) - (size_t)n,
                      ",\"firmware_version\":\"%s\"", firmware_ver);
    if (hw_model && hw_model[0])
        n += snprintf(body + n, sizeof(body) - (size_t)n,
                      ",\"hardware_model\":\"%s\"", hw_model);
    snprintf(body + n, sizeof(body) - (size_t)n, "}");

    char url[DEVICE_API_MAX_URL];
    snprintf(url, sizeof(url), "%s/devices/pair-codes", base_url);

    http_response_t raw;
    memset(&raw, 0, sizeof(raw));

    if (http_post(url, "application/json", body, &raw) < 0)
        return -1;

    /* Parse */
    char *s;
    s = device_api_json_get_string(raw.body, "device_id");
    if (s) { strncpy(resp->device_id, s, sizeof(resp->device_id) - 1); device_api_json_free(s); }

    s = device_api_json_get_string(raw.body, "code");
    if (s) { strncpy(resp->code, s, sizeof(resp->code) - 1); device_api_json_free(s); }

    s = device_api_json_get_string(raw.body, "pair_token");
    if (s) { strncpy(resp->pair_token, s, sizeof(resp->pair_token) - 1); device_api_json_free(s); }

    resp->expires_in_seconds = device_api_json_get_int(raw.body, "expires_in_seconds", 300);
    resp->poll_after_seconds = device_api_json_get_int(raw.body, "poll_after_seconds", 3);

    http_response_free(&raw);
    return 0;
}

int device_api_get_binding_status(const char *base_url,
                                  const char *device_id,
                                  const char *auth_header,
                                  device_binding_t *resp)
{
    if (!base_url || !device_id || !auth_header || !resp)
        return -1;
    memset(resp, 0, sizeof(*resp));

    char url[DEVICE_API_MAX_URL];
    snprintf(url, sizeof(url), "%s/devices/%s/binding-status",
             base_url, device_id);

    char extra_hdrs[DEVICE_API_MAX_TOKEN + 32];
    snprintf(extra_hdrs, sizeof(extra_hdrs), "Authorization: %s\r\n", auth_header);

    http_response_t raw;
    memset(&raw, 0, sizeof(raw));

    if (http_get_ex(url, extra_hdrs, &raw) < 0)
        return -1;

    /* Parse */
    char *s;
    s = device_api_json_get_string(raw.body, "status");
    if (s) { strncpy(resp->status, s, sizeof(resp->status) - 1); device_api_json_free(s); }

    s = device_api_json_get_string(raw.body, "device_token");
    if (s) { strncpy(resp->device_token, s, sizeof(resp->device_token) - 1); device_api_json_free(s); }

    s = device_api_json_get_string(raw.body, "agent_id");
    if (s) { strncpy(resp->agent_id, s, sizeof(resp->agent_id) - 1); device_api_json_free(s); }

    s = device_api_json_get_string(raw.body, "agent_name");
    if (s) { strncpy(resp->agent_name, s, sizeof(resp->agent_name) - 1); device_api_json_free(s); }

    resp->poll_after_seconds = device_api_json_get_int(raw.body, "poll_after_seconds", 30);

    http_response_free(&raw);
    return 0;
}

int device_api_start_conversation(const char *base_url,
                                  const char *device_id,
                                  const char *device_token,
                                  const char *body_params,
                                  device_conversation_t *resp)
{
    if (!base_url || !device_id || !device_token || !resp)
        return -1;
    memset(resp, 0, sizeof(*resp));

    char url[DEVICE_API_MAX_URL];
    snprintf(url, sizeof(url), "%s/devices/%s/conversations/start",
             base_url, device_id);

    /* Use provided body_params or default */
    const char *body = body_params ? body_params :
        "{\"trigger\":\"button\",\"audio\":{\"codec\":\"G722\",\"p_time\":20}}";

    char extra_hdrs[DEVICE_API_MAX_TOKEN + 32];
    snprintf(extra_hdrs, sizeof(extra_hdrs), "Authorization: Device %s\r\n", device_token);

    http_response_t raw;
    memset(&raw, 0, sizeof(raw));

    if (http_post_ex(url, "application/json", body, extra_hdrs, &raw) < 0)
        return -1;

    /* Parse */
    char *s;
    s = device_api_json_get_string(raw.body, "conversation_id");
    if (s) { strncpy(resp->conversation_id, s, sizeof(resp->conversation_id) - 1); device_api_json_free(s); }

    /* The RTC info is nested under "rtc": {"app_id":"...","channel":"...","token":"...","uid":...} */
    /* We need to extract from the nested JSON. Since our parser is flat, use a workaround. */
    /* Find the "rtc":{...} block and parse within it */
    const char *rtc_block = strstr(raw.body, "\"rtc\":{");
    if (rtc_block) {
        rtc_block += 6; /* skip "rtc":{ */

        /* Extract fields within the rtc block using nested key patterns */
        char key[64];

        snprintf(key, sizeof(key), "\"app_id\":\"");
        const char *vp = strstr(rtc_block, key);
        if (vp) {
            vp += strlen(key);
            const char *ve = strchr(vp, '"');
            if (ve) { size_t l = (size_t)(ve - vp);
                if (l < sizeof(resp->rtc_app_id)) { memcpy(resp->rtc_app_id, vp, l); resp->rtc_app_id[l] = '\0'; } }
        }

        snprintf(key, sizeof(key), "\"channel\":\"");
        vp = strstr(rtc_block, key);
        if (vp) {
            vp += strlen(key);
            const char *ve = strchr(vp, '"');
            if (ve) { size_t l = (size_t)(ve - vp);
                if (l < sizeof(resp->rtc_channel)) { memcpy(resp->rtc_channel, vp, l); resp->rtc_channel[l] = '\0'; } }
        }

        snprintf(key, sizeof(key), "\"token\":\"");
        vp = strstr(rtc_block, key);
        if (vp) {
            vp += strlen(key);
            const char *ve = strchr(vp, '"');
            if (ve) { size_t l = (size_t)(ve - vp);
                if (l < sizeof(resp->rtc_token)) { memcpy(resp->rtc_token, vp, l); resp->rtc_token[l] = '\0'; } }
        }

        snprintf(key, sizeof(key), "\"uid\":");
        vp = strstr(rtc_block, key);
        if (vp) {
            vp += strlen(key);
            while (*vp == ' ') vp++;
            int uid = 0;
            while (*vp >= '0' && *vp <= '9') uid = uid * 10 + (*vp++ - '0');
            resp->rtc_uid = uid;
        }
    }

    http_response_free(&raw);
    return 0;
}

int device_api_stop_conversation(const char *base_url,
                                 const char *device_id,
                                 const char *device_token,
                                 const char *conversation_id,
                                 const char *reason)
{
    if (!base_url || !device_id || !device_token || !conversation_id)
        return -1;

    char url[DEVICE_API_MAX_URL];
    snprintf(url, sizeof(url), "%s/devices/%s/conversations/stop",
             base_url, device_id);

    char body[512];
    snprintf(body, sizeof(body),
        "{\"conversation_id\":\"%s\"", conversation_id);
    if (reason) {
        size_t bl = strlen(body);
        snprintf(body + bl, sizeof(body) - bl,
                 ",\"reason\":\"%s\"", reason);
    }
    size_t bl = strlen(body);
    snprintf(body + bl, sizeof(body) - bl, "}");

    char extra_hdrs[DEVICE_API_MAX_TOKEN + 32];
    snprintf(extra_hdrs, sizeof(extra_hdrs), "Authorization: Device %s\r\n", device_token);

    http_response_t raw;
    memset(&raw, 0, sizeof(raw));

    int ret = http_post_ex(url, "application/json", body, extra_hdrs, &raw);
    http_response_free(&raw);
    return ret;
}
