/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot_provisioning.h>

#include "mybot_wifi_credentials.h"
#include "mybot_wifi_runtime.h"

#include <mybot_event.h>

#include <common/bk_err.h>
#include "mybot_platform_log.h"
#include <components/netif.h>
#include <components/netif_types.h>
#include <modules/wifi.h>
#include <modules/wifi_types.h>
#include <os/mem.h>
#include <os/os.h>

#include "cJSON.h"
#include <lwip/inet.h>
#include <lwip/sockets.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PROV_TAG "mybot_pr"
#define PROV_LOGI(format, ...) MYBOT_LOGI(PROV_TAG, format, ##__VA_ARGS__)
#define PROV_LOGW(format, ...) MYBOT_LOGW(PROV_TAG, format, ##__VA_ARGS__)
#define PROV_LOGE(format, ...) MYBOT_LOGE(PROV_TAG, format, ##__VA_ARGS__)

#define BK725X_APSTA_HTTP_PORT 80
#define BK725X_APSTA_SELECT_TIMEOUT_MS 200
#define BK725X_APSTA_HTTP_IDLE_TIMEOUT_MS 15000u
#define BK725X_APSTA_HTTP_HEADER_MAX 2048u
#define BK725X_APSTA_HTTP_BODY_MAX 1024u
#define BK725X_APSTA_CONNECT_TIMEOUT_MS 10000u
#define BK725X_APSTA_CONNECT_RETRY_DELAY_MS 3000u
#define BK725X_APSTA_SCAN_TIMEOUT_MS 15000u
#define BK725X_APSTA_SCAN_STOP_RETRY_MS 1000u
#define BK725X_APSTA_SCAN_INTERVAL_MS 10000u
#define BK725X_APSTA_EXIT_DELAY_MS 200u
#define BK725X_APSTA_AP_CHANNEL 1
#define BK725X_APSTA_MAX_SCAN_RESULTS 32
#define BK725X_APSTA_JSON_CAPACITY 12288u

typedef struct {
    char method[8];
    char target[192];
    char body[BK725X_APSTA_HTTP_BODY_MAX + 1];
    size_t body_length;
} bk725x_http_request_t;

typedef struct {
    mybot_wifi_runtime_t wifi;
    bool ap_started;
    beken_thread_t worker_thread;
    bool worker_started;
    uint32_t last_scan_time;
    uint32_t scan_deadline;
    uint32_t event_generation;
    char ap_ssid[WIFI_SSID_STR_LEN];
    mybot_wifi_credential_list_t credentials;
    wifi_scan_ap_info_t scan_results[BK725X_APSTA_MAX_SCAN_RESULTS];
    size_t scan_result_count;
} bk725x_wifi_apsta_ctx_t;

static bk725x_wifi_apsta_ctx_t *s_active_ctx;
static beken_mutex_t s_state_lock;
static bool s_json_hooks_initialized;
static mybot_provisioning_state_t s_provisioning_state = MYBOT_PROVISIONING_STATE_IDLE;

static const char s_configuration_html[] =
    "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' "
    "content='width=device-width,initial-scale=1'><title>Network "
    "Configuration</title>"
    "<style>*{box-sizing:border-box}body{font:15px "
    "system-ui;margin:0;background:#f3f5f7;"
    "color:#17202a}.page{max-width:480px;margin:auto;padding:24px "
    "16px}section{background:#fff;"
    "border:1px solid "
    "#dfe4e8;border-radius:8px;padding:18px;margin-bottom:12px}h1{font-size:"
    "22px;"
    "margin:0 0 16px}h2{font-size:15px;margin:0 0 "
    "12px}label{display:block;margin:10px 0 5px}"
    "select,input,button{width:100%;min-height:42px;padding:9px;border:1px "
    "solid #aeb8c2;"
    "border-radius:6px;background:#fff}button{margin-top:12px;background:#"
    "1769aa;color:#fff;"
    "border:0;font-weight:600}button:disabled{background:#8c9aa6}.saved{"
    "display:flex;gap:6px;"
    "align-items:center;padding:7px 0;border-bottom:1px solid #edf0f2}.saved "
    "span{flex:1;"
    "overflow:hidden;text-overflow:ellipsis}.saved "
    "button{width:auto;min-height:32px;margin:0;"
    "padding:5px 9px;background:#5b6670}.saved "
    ".del{background:#b3261e}#status{min-height:20px;"
    "color:#5b6670}.error{color:#b3261e!important}</style></head><body><main "
    "class='page'>"
    "<h1>Wi-Fi configuration</h1><section><h2>Connect to a network</h2><form "
    "id='form'>"
    "<label for='networks'>Network</label><select "
    "id='networks'></select><label for='ssid'>"
    "SSID</label><input id='ssid' maxlength='32' required><label "
    "for='password'>Password</label>"
    "<input id='password' type='password' maxlength='64'><button "
    "id='submit'>Connect</button>"
    "</form><p id='status'></p></section><section><h2>Saved networks</h2><div "
    "id='saved'>"
    "</div></section></main><script>const q=s=>document.querySelector(s);async "
    "function scan(){try{"
    "const d=await "
    "fetch('/scan',{cache:'no-store'}).then(r=>r.json()),sel=q('#networks');"
    "sel.innerHTML='<option value=\"\">Manual "
    "entry</option>';d.aps.forEach(a=>{const o="
    "document.createElement('option');o.value=a.ssid;o.textContent=a.ssid+' "
    "('+a.rssi+' dBm)';"
    "sel.appendChild(o)})}catch(e){}}async function saved(){try{const d=await "
    "fetch('/saved/list',"
    "{cache:'no-store'}).then(r=>r.json()),box=q('#saved');box.innerHTML='';d."
    "forEach((s,i)=>{"
    "const row=document.createElement('div');row.className='saved';const "
    "name=document.createElement"
    "('span');name.textContent=s;row.appendChild(name);[['Use','/saved/"
    "set_default?index='+i,''],"
    "['Delete','/saved/delete?index='+i,'del']].forEach(x=>{const "
    "b=document.createElement('button');"
    "b.type='button';b.textContent=x[0];b.className=x[2];b.onclick=async()=>{"
    "await fetch(x[1]);"
    "saved()};row.appendChild(b)});box.appendChild(row)})}catch(e){}}q('#"
    "networks').onchange=e=>"
    "{if(e.target.value)q('#ssid').value=e.target.value};q('#form').onsubmit="
    "async e=>{e.preventDefault();"
    "const "
    "b=q('#submit'),st=q('#status');b.disabled=true;st.className='';st."
    "textContent='Testing "
    "the connection...';try{const r=await "
    "fetch('/submit',{method:'POST',headers:{'Content-Type':"
    "'application/"
    "json'},body:JSON.stringify({ssid:q('#ssid').value,password:q('#password')."
    "value})});"
    "const d=await r.json();if(d.success){location.href='/done.html'}else "
    "throw Error(d.error||"
    "'Connection "
    "failed')}catch(x){st.className='error';st.textContent=x.message;b."
    "disabled=false}};"
    "scan();saved();setInterval(scan,5000)</script></body></html>";

static const char s_done_html[] =
    "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' "
    "content='width=device-width,initial-scale=1'><title>Configuration "
    "successful</title>"
    "<style>body{font:16px "
    "system-ui;display:grid;place-items:center;height:100vh;margin:0;"
    "background:#f3f5f7;color:#17202a}.box{text-align:center;background:#fff;"
    "border:1px solid "
    "#dfe4e8;border-radius:8px;padding:36px}.ok{font-size:48px;color:#168344}</"
    "style></head>"
    "<body><div class='box'><div class='ok'>&#10003;</div><h2>Configuration "
    "successful</h2>"
    "</div><script>setTimeout(()=>fetch('/exit',{method:'POST'}),1000)</"
    "script></body></html>";

static bool state_lock_acquire(void) {
    if (!s_state_lock && rtos_init_mutex(&s_state_lock) != BK_OK) {
        return false;
    }
    return rtos_lock_mutex(&s_state_lock) == BK_OK;
}

static void state_lock_release(void) {
    rtos_unlock_mutex(&s_state_lock);
}

static void set_provisioning_state(mybot_provisioning_state_t state) {
    if (state_lock_acquire()) {
        s_provisioning_state = state;
        state_lock_release();
    } else {
        PROV_LOGE("[prov] failed to update provisioning state");
    }
}

static bool is_stopping(bk725x_wifi_apsta_ctx_t *ctx) {
    return mybot_wifi_runtime_is_stopping(&ctx->wifi);
}

static bool credential_lengths_are_valid(size_t ssid_length, size_t password_length) {
    return ssid_length > 0 && ssid_length < WIFI_SSID_STR_LEN &&
           password_length < WIFI_PASSWORD_LEN;
}

static void *json_psram_malloc(size_t size) {
    return psram_malloc(size);
}

static void json_psram_free(void *memory) {
    psram_free(memory);
}

static int initialize_json_hooks_once(void) {
    if (!state_lock_acquire()) {
        return -1;
    }
    if (!s_json_hooks_initialized) {
        cJSON_Hooks hooks = {
            .malloc_fn = json_psram_malloc,
            .free_fn = json_psram_free,
        };
        cJSON_InitHooks(&hooks);
        s_json_hooks_initialized = true;
    }
    state_lock_release();
    return 0;
}

static void build_ap_ssid(char output[WIFI_SSID_STR_LEN], const char *device_id) {
    uint8_t mac[WIFI_MAC_LEN] = {0};
    if (bk_wifi_ap_get_mac(mac) == BK_OK) {
        snprintf(output, WIFI_SSID_STR_LEN, "mybot-%01x%02x", mac[4] & 0x0f, mac[5]);
        return;
    }

    size_t device_id_length = strlen(device_id);
    const char *suffix = device_id_length > 3 ? device_id + device_id_length - 3 : device_id;
    snprintf(output, WIFI_SSID_STR_LEN, "mybot-%.3s", suffix);
}

static int start_softap(bk725x_wifi_apsta_ctx_t *ctx) {
    if (is_stopping(ctx) || ctx->ap_started) {
        return 0;
    }

    netif_ip4_config_t ip4_config = {0};
    snprintf(ip4_config.ip, sizeof(ip4_config.ip), "%s", "192.168.4.1");
    snprintf(ip4_config.mask, sizeof(ip4_config.mask), "%s", "255.255.255.0");
    snprintf(ip4_config.gateway, sizeof(ip4_config.gateway), "%s", "192.168.4.1");
    snprintf(ip4_config.dns, sizeof(ip4_config.dns), "%s", "192.168.4.1");
    if (bk_netif_set_ip4_config(NETIF_IF_AP, &ip4_config) != BK_OK) {
        PROV_LOGE("[prov] failed to configure SoftAP IPv4");
        return -1;
    }

    wifi_ap_config_t ap_config = {0};
    snprintf(ap_config.ssid, sizeof(ap_config.ssid), "%s", ctx->ap_ssid);
    ap_config.channel = BK725X_APSTA_AP_CHANNEL;
    ap_config.security = WIFI_SECURITY_NONE;
    ap_config.max_con = 2;
    ap_config.disable_dns_server = 0;
    if (bk_wifi_ap_set_config(&ap_config) != BK_OK || bk_wifi_ap_start() != BK_OK) {
        PROV_LOGE("[prov] failed to start configuration SoftAP");
        return -1;
    }

    ctx->ap_started = true;
    return 0;
}

static bool stop_softap(bk725x_wifi_apsta_ctx_t *ctx) {
    if (!ctx->ap_started) {
        return true;
    }
    if (bk_wifi_ap_stop() != BK_OK) {
        PROV_LOGW("[prov] failed to stop configuration SoftAP");
        return false;
    }
    ctx->ap_started = false;
    return true;
}

static bool stop_sta(bk725x_wifi_apsta_ctx_t *ctx) {
    return mybot_wifi_runtime_stop_sta(&ctx->wifi);
}

static int start_sta(bk725x_wifi_apsta_ctx_t *ctx,
                     const mybot_wifi_credential_entry_t *credential) {
    return mybot_wifi_runtime_start_sta(&ctx->wifi, credential, 1);
}

static bool wait_for_sta_ipv4(bk725x_wifi_apsta_ctx_t *ctx, const char *ssid, uint32_t timeout_ms) {
    return mybot_wifi_runtime_wait_for_ipv4(&ctx->wifi, ssid, timeout_ms);
}

static int collect_scan_results(bk725x_wifi_apsta_ctx_t *ctx) {
    int result = mybot_wifi_runtime_collect_scan_results(
        &ctx->wifi, ctx->scan_results, BK725X_APSTA_MAX_SCAN_RESULTS,
        &ctx->scan_result_count);
    ctx->last_scan_time = mybot_wifi_time_now();
    ctx->scan_deadline = 0;
    return result;
}

static int start_scan(bk725x_wifi_apsta_ctx_t *ctx) {
    int result = mybot_wifi_runtime_start_scan(&ctx->wifi);
    if (result == 0) {
        ctx->scan_deadline = mybot_wifi_time_now() + BK725X_APSTA_SCAN_TIMEOUT_MS;
    }
    return result;
}

static bool stop_scan(bk725x_wifi_apsta_ctx_t *ctx) {
    bool stopped = mybot_wifi_runtime_stop_scan(&ctx->wifi);
    if (stopped) {
        ctx->scan_deadline = 0;
    }
    return stopped;
}

static bool test_credentials(bk725x_wifi_apsta_ctx_t *ctx,
                             const mybot_wifi_credential_entry_t *credential) {
    if (!stop_scan(ctx)) {
        PROV_LOGW("[prov] cannot test credentials while scan is active");
        return false;
    }
    for (int attempt = 0; attempt < 2 && !is_stopping(ctx); ++attempt) {
        if (attempt > 0 &&
            !mybot_wifi_runtime_delay(&ctx->wifi, BK725X_APSTA_CONNECT_RETRY_DELAY_MS)) {
            break;
        }
        if (start_sta(ctx, credential) == 0 &&
            wait_for_sta_ipv4(ctx, credential->ssid, BK725X_APSTA_CONNECT_TIMEOUT_MS)) {
            if (!stop_sta(ctx)) {
                break;
            }
            ctx->last_scan_time = 0;
            return true;
        }
        (void)stop_sta(ctx);
    }
    ctx->last_scan_time = 0;
    return false;
}

static int wait_readable(int fd) {
    fd_set readfds;
    struct timeval timeout = {
        .tv_sec = BK725X_APSTA_SELECT_TIMEOUT_MS / 1000,
        .tv_usec = (BK725X_APSTA_SELECT_TIMEOUT_MS % 1000) * 1000,
    };

    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);
    return select(fd + 1, &readfds, NULL, NULL, &timeout);
}

static int send_all(int fd, const void *data, size_t length) {
    const uint8_t *cursor = (const uint8_t *)data;
    while (length > 0) {
        int sent = send(fd, cursor, length, 0);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent <= 0) {
            return -1;
        }
        cursor += sent;
        length -= (size_t)sent;
    }
    return 0;
}

static int send_http_response(int fd, const char *status, const char *content_type,
                              const char *extra_headers, const char *body, size_t body_length) {
    char header[384];
    int header_length =
        snprintf(header, sizeof(header),
                 "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: "
                 "%u\r\nCache-Control: no-store\r\n"
                 "Connection: close\r\n%s\r\n",
                 status, content_type, (unsigned)body_length, extra_headers ? extra_headers : "");
    if (header_length < 0 || (size_t)header_length >= sizeof(header) ||
        send_all(fd, header, (size_t)header_length) < 0) {
        return -1;
    }
    return body_length == 0 || send_all(fd, body, body_length) == 0 ? 0 : -1;
}

static int parse_content_length(const char *value, size_t *content_length) {
    while (*value == ' ' || *value == '\t') {
        ++value;
    }
    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    while (end && (*end == ' ' || *end == '\t')) {
        ++end;
    }
    if (errno != 0 || !end || (*end != '\0' && *end != '\r') ||
        parsed > BK725X_APSTA_HTTP_BODY_MAX) {
        return -1;
    }
    *content_length = (size_t)parsed;
    return 0;
}

static int receive_http_request(bk725x_wifi_apsta_ctx_t *ctx, int fd,
                                bk725x_http_request_t *request) {
    const size_t capacity = BK725X_APSTA_HTTP_HEADER_MAX + BK725X_APSTA_HTTP_BODY_MAX;
    char *buffer = psram_malloc(capacity + 1);
    if (!buffer) {
        return -1;
    }

    int result = -1;
    size_t received_length = 0;
    size_t required_length = 0;
    size_t header_length = 0;
    uint32_t last_activity = mybot_wifi_time_now();

    while (!is_stopping(ctx) && received_length < capacity) {
        int ready = wait_readable(fd);
        if (ready == 0) {
            if ((uint32_t)(mybot_wifi_time_now() - last_activity) >=
                BK725X_APSTA_HTTP_IDLE_TIMEOUT_MS) {
                break;
            }
            continue;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        int count = recv(fd, buffer + received_length, capacity - received_length, 0);
        if (count <= 0) {
            break;
        }
        received_length += (size_t)count;
        buffer[received_length] = '\0';
        last_activity = mybot_wifi_time_now();

        if (header_length == 0) {
            char *header_end = strstr(buffer, "\r\n\r\n");
            if (!header_end) {
                if (received_length >= BK725X_APSTA_HTTP_HEADER_MAX) {
                    break;
                }
                continue;
            }
            header_length = (size_t)(header_end - buffer) + 4;

            char *request_line_end = strstr(buffer, "\r\n");
            char version[16] = {0};
            if (!request_line_end) {
                break;
            }
            *request_line_end = '\0';
            if (sscanf(buffer, "%7s %191s %15s", request->method, request->target, version) != 3 ||
                strncmp(version, "HTTP/1.", 7) != 0) {
                break;
            }

            size_t content_length = 0;
            char *line = request_line_end + 2;
            while (line < header_end) {
                char *line_end = strstr(line, "\r\n");
                if (!line_end || line_end > header_end) {
                    break;
                }
                *line_end = '\0';
                if (strncasecmp(line, "Content-Length:", 15) == 0 &&
                    parse_content_length(line + 15, &content_length) < 0) {
                    goto done;
                }
                line = line_end + 2;
            }
            required_length = header_length + content_length;
            if (required_length > capacity) {
                break;
            }
            request->body_length = content_length;
        }

        if (header_length != 0 && received_length >= required_length) {
            if (request->body_length > 0) {
                memcpy(request->body, buffer + header_length, request->body_length);
            }
            request->body[request->body_length] = '\0';
            result = 0;
            break;
        }
    }

done:
    psram_free(buffer);
    return result;
}

static int security_to_authmode(wifi_security_t security) {
    switch (security) {
    case WIFI_SECURITY_NONE:
        return 0;
    case WIFI_SECURITY_WEP:
        return 1;
    case WIFI_SECURITY_WPA_TKIP:
    case WIFI_SECURITY_WPA_AES:
    case WIFI_SECURITY_WPA_MIXED:
        return 2;
    case WIFI_SECURITY_WPA2_TKIP:
    case WIFI_SECURITY_WPA2_AES:
    case WIFI_SECURITY_WPA2_MIXED:
        return 3;
    case WIFI_SECURITY_WPA3_SAE:
        return 6;
    case WIFI_SECURITY_WPA3_WPA2_MIXED:
        return 7;
    case WIFI_SECURITY_EAP:
        return 5;
    case WIFI_SECURITY_OWE:
        return 9;
    default:
        return 3;
    }
}

static char *build_scan_json(const bk725x_wifi_apsta_ctx_t *ctx) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON *aps = NULL;
    if (!cJSON_AddBoolToObject(root, "support_5g", false) ||
        !(aps = cJSON_AddArrayToObject(root, "aps"))) {
        cJSON_Delete(root);
        return NULL;
    }

    for (size_t i = 0; i < ctx->scan_result_count; ++i) {
        cJSON *ap = cJSON_CreateObject();
        if (!ap || !cJSON_AddStringToObject(ap, "ssid", ctx->scan_results[i].ssid) ||
            !cJSON_AddNumberToObject(ap, "rssi", ctx->scan_results[i].rssi) ||
            !cJSON_AddNumberToObject(ap, "authmode",
                                     security_to_authmode(ctx->scan_results[i].security))) {
            cJSON_Delete(ap);
            cJSON_Delete(root);
            return NULL;
        }
        if (!cJSON_AddItemToArray(aps, ap)) {
            cJSON_Delete(ap);
            cJSON_Delete(root);
            return NULL;
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json && strlen(json) >= BK725X_APSTA_JSON_CAPACITY) {
        cJSON_free(json);
        return NULL;
    }
    return json;
}

static char *build_saved_json(const mybot_wifi_credential_list_t *list) {
    cJSON *root = cJSON_CreateArray();
    if (!root) {
        return NULL;
    }

    for (size_t i = 0; i < list->count; ++i) {
        cJSON *ssid = cJSON_CreateString(list->entries[i].ssid);
        if (!ssid || !cJSON_AddItemToArray(root, ssid)) {
            cJSON_Delete(ssid);
            cJSON_Delete(root);
            return NULL;
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json && strlen(json) >= BK725X_APSTA_JSON_CAPACITY) {
        cJSON_free(json);
        return NULL;
    }
    return json;
}

static int parse_submit_json(const char *input, char ssid[WIFI_SSID_STR_LEN],
                             char password[WIFI_PASSWORD_LEN]) {
    bool have_ssid = false;
    bool have_password = false;
    int result = -1;

    ssid[0] = '\0';
    password[0] = '\0';
    if (!input) {
        return -1;
    }

    cJSON *root = cJSON_ParseWithOpts(input, NULL, true);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -1;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        char *output = NULL;
        size_t capacity = 0;
        bool *present = NULL;

        if (!item->string || !cJSON_IsString(item)) {
            goto done;
        }
        if (strcmp(item->string, "ssid") == 0) {
            output = ssid;
            capacity = WIFI_SSID_STR_LEN;
            present = &have_ssid;
        } else if (strcmp(item->string, "password") == 0) {
            output = password;
            capacity = WIFI_PASSWORD_LEN;
            present = &have_password;
        } else {
            goto done;
        }

        if (*present) {
            goto done;
        }
        size_t length = strlen(item->valuestring);
        if (length >= capacity) {
            goto done;
        }
        memcpy(output, item->valuestring, length + 1);
        *present = true;
    }

    result = have_ssid ? 0 : -1;

done:
    cJSON_Delete(root);
    return result;
}

static int parse_query_index(const char *target, size_t *index) {
    const char *value = strstr(target, "?index=");
    if (!value) {
        return -1;
    }
    value += 7;
    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || (*end != '\0' && *end != '&') ||
        parsed >= MYBOT_WIFI_MAX_CREDENTIALS) {
        return -1;
    }
    *index = (size_t)parsed;
    return 0;
}

static int handle_submit(bk725x_wifi_apsta_ctx_t *ctx, int fd,
                         const bk725x_http_request_t *request) {
    char ssid[WIFI_SSID_STR_LEN];
    char password[WIFI_PASSWORD_LEN];
    if (parse_submit_json(request->body, ssid, password) < 0) {
        static const char invalid[] = "{\"success\":false,\"error\":\"Invalid SSID or password\"}";
        return send_http_response(fd, "200 OK", "application/json", NULL, invalid,
                                  sizeof(invalid) - 1);
    }

    size_t ssid_length = strlen(ssid);
    size_t password_length = strlen(password);
    if (!credential_lengths_are_valid(ssid_length, password_length)) {
        static const char invalid[] = "{\"success\":false,\"error\":\"Invalid SSID or password\"}";
        return send_http_response(fd, "200 OK", "application/json", NULL, invalid,
                                  sizeof(invalid) - 1);
    }
    PROV_LOGI("[prov] credentials submitted: ssid=%s", ssid);
    mybot_wifi_credential_entry_t candidate = {0};
    candidate.ssid_length = (uint8_t)ssid_length;
    candidate.password_length = (uint8_t)password_length;
    memcpy(candidate.ssid, ssid, ssid_length + 1);
    memcpy(candidate.password, password, password_length + 1);

    if (!test_credentials(ctx, &candidate)) {
        static const char failed[] = "{\"success\":false,\"error\":\"Failed to "
                                     "connect to the Access Point\"}";
        return send_http_response(fd, "200 OK", "application/json", NULL, failed,
                                  sizeof(failed) - 1);
    }

    mybot_wifi_credential_list_t previous = ctx->credentials;
    if (mybot_wifi_credentials_add(&ctx->credentials, candidate.ssid, candidate.password) < 0 ||
        mybot_wifi_credentials_save(&ctx->credentials) < 0) {
        ctx->credentials = previous;
        static const char failed[] =
            "{\"success\":false,\"error\":\"Failed to save Wi-Fi credentials\"}";
        return send_http_response(fd, "200 OK", "application/json", NULL, failed,
                                  sizeof(failed) - 1);
    }

    PROV_LOGI("[prov] credentials verified and saved: ssid=%s", candidate.ssid);
    static const char success[] = "{\"success\":true}";
    return send_http_response(fd, "200 OK", "application/json", NULL, success, sizeof(success) - 1);
}

static bool target_is_captive_probe(const char *target) {
    static const char *const probes[] = {
        "/hotspot-detect.html",       "/generate_204", "/mobile/status.php",
        "/check_network_status.txt",  "/ncsi.txt",     "/fwlink/",
        "/connectivity-check.html",   "/success.txt",  "/portal.html",
        "/library/test/success.html",
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) {
        if (strncmp(target, probes[i], strlen(probes[i])) == 0) {
            return true;
        }
    }
    return false;
}

static void handle_http_client(bk725x_wifi_apsta_ctx_t *ctx, int fd, bool *exit_requested) {
    bk725x_http_request_t request = {0};
    if (receive_http_request(ctx, fd, &request) < 0) {
        static const char bad_request[] = "Bad Request";
        send_http_response(fd, "400 Bad Request", "text/plain", NULL, bad_request,
                           sizeof(bad_request) - 1);
        return;
    }

    if (strcmp(request.method, "GET") == 0 &&
        (strcmp(request.target, "/") == 0 || strncmp(request.target, "/?", 2) == 0)) {
        send_http_response(fd, "200 OK", "text/html; charset=utf-8", NULL, s_configuration_html,
                           sizeof(s_configuration_html) - 1);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.target, "/scan") == 0) {
        char *json = build_scan_json(ctx);
        if (json) {
            send_http_response(fd, "200 OK", "application/json", NULL, json, strlen(json));
            cJSON_free(json);
        } else {
            send_http_response(fd, "500 Internal Server Error", "application/json", NULL, "{}", 2);
        }
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.target, "/saved/list") == 0) {
        char *json = build_saved_json(&ctx->credentials);
        if (json) {
            send_http_response(fd, "200 OK", "application/json", NULL, json, strlen(json));
            cJSON_free(json);
        } else {
            send_http_response(fd, "500 Internal Server Error", "application/json", NULL, "[]", 2);
        }
    } else if (strcmp(request.method, "GET") == 0 &&
               strncmp(request.target, "/saved/set_default?", 19) == 0) {
        size_t index;
        mybot_wifi_credential_list_t previous = ctx->credentials;
        if (parse_query_index(request.target, &index) < 0 ||
            mybot_wifi_credentials_set_default(&ctx->credentials, index) < 0 ||
            mybot_wifi_credentials_save(&ctx->credentials) < 0) {
            ctx->credentials = previous;
            send_http_response(fd, "400 Bad Request", "application/json", NULL, "{}", 2);
        } else {
            send_http_response(fd, "200 OK", "application/json", NULL, "{}", 2);
        }
    } else if (strcmp(request.method, "GET") == 0 &&
               strncmp(request.target, "/saved/delete?", 14) == 0) {
        size_t index;
        mybot_wifi_credential_list_t previous = ctx->credentials;
        if (parse_query_index(request.target, &index) < 0 ||
            mybot_wifi_credentials_delete(&ctx->credentials, index) < 0 ||
            mybot_wifi_credentials_save(&ctx->credentials) < 0) {
            ctx->credentials = previous;
            send_http_response(fd, "400 Bad Request", "application/json", NULL, "{}", 2);
        } else {
            send_http_response(fd, "200 OK", "application/json", NULL, "{}", 2);
        }
    } else if (strcmp(request.method, "POST") == 0 && strcmp(request.target, "/submit") == 0) {
        handle_submit(ctx, fd, &request);
    } else if (strcmp(request.method, "GET") == 0 &&
               strncmp(request.target, "/done.html", 10) == 0) {
        send_http_response(fd, "200 OK", "text/html; charset=utf-8", NULL, s_done_html,
                           sizeof(s_done_html) - 1);
    } else if (strcmp(request.method, "POST") == 0 && strcmp(request.target, "/exit") == 0) {
        static const char success[] = "{\"success\":true}";
        send_http_response(fd, "200 OK", "application/json", NULL, success, sizeof(success) - 1);
        *exit_requested = true;
    } else if (strcmp(request.method, "GET") == 0 && target_is_captive_probe(request.target)) {
        send_http_response(fd, "302 Found", "text/html", "Location: http://192.168.4.1/\r\n", NULL,
                           0);
    } else {
        static const char not_found[] = "Not Found";
        send_http_response(fd, "404 Not Found", "text/plain", NULL, not_found,
                           sizeof(not_found) - 1);
    }
}

static int create_http_server(void) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }
    int reuse_address = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(BK725X_APSTA_HTTP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(fd, 2) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int run_configuration_portal(bk725x_wifi_apsta_ctx_t *ctx) {
    int listen_fd = -1;
    int result = -1;

    (void)stop_sta(ctx);
    ctx->scan_result_count = 0;
    if (start_softap(ctx) < 0) {
        goto out;
    }

    listen_fd = create_http_server();
    if (listen_fd < 0) {
        PROV_LOGE("[prov] failed to start configuration HTTP server, errno=%d", errno);
        goto out;
    }
    PROV_LOGI("[prov] APSTA portal ready: ssid=%s url=http://192.168.4.1", ctx->ap_ssid);

    start_scan(ctx);
    bool exit_requested = false;
    uint32_t exit_deadline = 0;
    while (!is_stopping(ctx)) {
        uint32_t flags =
            mybot_wifi_runtime_take_events(&ctx->wifi, MYBOT_WIFI_EVENT_SCAN_DONE);
        if ((flags & MYBOT_WIFI_EVENT_SCAN_DONE) != 0) {
            collect_scan_results(ctx);
        }

        uint32_t now = mybot_wifi_time_now();
        if (mybot_wifi_runtime_is_scan_in_progress(&ctx->wifi) &&
            mybot_wifi_time_reached(now, ctx->scan_deadline)) {
            PROV_LOGW("[prov] asynchronous scan timed out");
            if (stop_scan(ctx)) {
                ctx->last_scan_time = now;
            } else {
                ctx->scan_deadline = now + BK725X_APSTA_SCAN_STOP_RETRY_MS;
            }
        }
        if (!exit_requested && !mybot_wifi_runtime_is_scan_in_progress(&ctx->wifi) &&
            (ctx->last_scan_time == 0 ||
             (uint32_t)(now - ctx->last_scan_time) >= BK725X_APSTA_SCAN_INTERVAL_MS)) {
            start_scan(ctx);
        }
        if (exit_requested && mybot_wifi_time_reached(now, exit_deadline)) {
            result = 0;
            break;
        }

        int ready = wait_readable(listen_fd);
        if (ready == 0) {
            continue;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            PROV_LOGE("[prov] configuration server select failed, errno=%d", errno);
            goto out;
        }

        struct sockaddr_in client_address = {0};
        socklen_t client_address_length = sizeof(client_address);
        int client_fd =
            accept(listen_fd, (struct sockaddr *)&client_address, &client_address_length);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED) {
                continue;
            }
            PROV_LOGE("[prov] configuration server accept failed, errno=%d", errno);
            goto out;
        }
        struct timeval send_timeout = {
            .tv_sec = 1,
            .tv_usec = 0,
        };
        if (setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout)) <
            0) {
            PROV_LOGE("[prov] failed to set HTTP client send timeout, errno=%d", errno);
            shutdown(client_fd, SHUT_RDWR);
            close(client_fd);
            continue;
        }
        bool request_exit = false;
        handle_http_client(ctx, client_fd, &request_exit);
        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
        if (request_exit) {
            exit_requested = true;
            exit_deadline = mybot_wifi_time_now() + BK725X_APSTA_EXIT_DELAY_MS;
        }
    }

out:
    if (listen_fd >= 0) {
        close(listen_fd);
    }
    PROV_LOGI("[prov] APSTA portal exit: result=%d stopping=%d", result,
              is_stopping(ctx) ? 1 : 0);
    return is_stopping(ctx) ? -1 : result;
}

static void wifi_worker(beken_thread_arg_t arg) {
    bk725x_wifi_apsta_ctx_t *ctx = (bk725x_wifi_apsta_ctx_t *)arg;
    PROV_LOGI("[prov] APSTA worker started: saved=%u", (unsigned)ctx->credentials.count);
    int result = run_configuration_portal(ctx);

    bool wifi_released = stop_scan(ctx);
    wifi_released = stop_softap(ctx) && wifi_released;
    wifi_released = stop_sta(ctx) && wifi_released;
    if (!wifi_released) {
        result = -1;
        PROV_LOGE("[prov] APSTA worker could not release Wi-Fi ownership");
    }
    if (!is_stopping(ctx)) {
        mybot_event_type_t event = result == 0 ? MYBOT_EVENT_PROVISIONING_COMPLETED
                                               : MYBOT_EVENT_PROVISIONING_FAILED;
        set_provisioning_state(result == 0 ? MYBOT_PROVISIONING_STATE_COMPLETED
                                           : MYBOT_PROVISIONING_STATE_FAILED);
        if (mybot_event_post_with_generation(event, ctx->event_generation) != 0) {
            PROV_LOGE("[prov] failed to post provisioning result: result=%d", result);
        }
    }
    PROV_LOGI("[prov] APSTA worker stopped: result=%d", result);
    rtos_delete_thread(NULL);
}

static void destroy_context_resources(bk725x_wifi_apsta_ctx_t *ctx) {
    mybot_wifi_runtime_deinit(&ctx->wifi);
    psram_free(ctx);
}

static int start_context(const char *device_id, uint32_t generation) {
    if (!device_id || !device_id[0] || generation == 0) {
        return -1;
    }

    PROV_LOGI("[prov] context start: device=%s", device_id);
    bk725x_wifi_apsta_ctx_t *ctx = psram_zalloc(sizeof(*ctx));
    if (!ctx) {
        return -1;
    }
    ctx->event_generation = generation;
    if (s_active_ctx) {
        psram_free(ctx);
        return -1;
    }
    set_provisioning_state(MYBOT_PROVISIONING_STATE_RUNNING);

    build_ap_ssid(ctx->ap_ssid, device_id);
    if (initialize_json_hooks_once() < 0) {
        PROV_LOGE("[prov] failed to initialize cJSON hooks");
        goto failed;
    }
    if (mybot_wifi_credentials_load(&ctx->credentials) < 0) {
        PROV_LOGE("[prov] failed to load saved credentials");
        goto failed;
    }
    if (mybot_wifi_runtime_init(&ctx->wifi) < 0) {
        PROV_LOGE("[prov] failed to create Wi-Fi runtime");
        goto failed;
    }
    if (mybot_wifi_runtime_register_callbacks(&ctx->wifi) < 0) {
        goto failed;
    }

    if (rtos_create_psram_thread(&ctx->worker_thread, 2, "mybot_provision", wifi_worker, 8192,
                                 (beken_thread_arg_t)ctx) != BK_OK) {
        PROV_LOGE("[prov] worker thread creation failed");
        goto failed;
    }
    ctx->worker_started = true;
    s_active_ctx = ctx;
    PROV_LOGI("[prov] started: ap_ssid=%s saved=%u", ctx->ap_ssid,
              (unsigned)ctx->credentials.count);
    return 0;

failed:
    PROV_LOGE("[prov] context start failed; cleaning up");
    mybot_wifi_runtime_request_stop(&ctx->wifi);
    bool callbacks_detached = mybot_wifi_runtime_unregister_callbacks(&ctx->wifi);
    if (ctx->worker_started) {
        rtos_thread_join(&ctx->worker_thread);
        ctx->worker_started = false;
    }
    bool wifi_released = stop_scan(ctx);
    wifi_released = stop_softap(ctx) && wifi_released;
    wifi_released = stop_sta(ctx) && wifi_released;
    bool retain_context = !wifi_released || !callbacks_detached;
    if (retain_context) {
        s_active_ctx = ctx;
        set_provisioning_state(MYBOT_PROVISIONING_STATE_FAILED);
    } else {
        s_active_ctx = NULL;
        set_provisioning_state(MYBOT_PROVISIONING_STATE_IDLE);
    }

    if (retain_context) {
        if (!wifi_released) {
            PROV_LOGE("[prov] failed startup cleanup retained Wi-Fi ownership");
        }
        if (!callbacks_detached) {
            PROV_LOGE("event callbacks remain registered after start failure; retaining context");
        }
        return -1;
    }
    destroy_context_resources(ctx);
    return -1;
}

int mybot_provisioning_start(const char *device_id, uint32_t generation) {
    return start_context(device_id, generation);
}

int mybot_provisioning_stop(void) {
    bk725x_wifi_apsta_ctx_t *ctx = s_active_ctx;
    if (!ctx) {
        set_provisioning_state(MYBOT_PROVISIONING_STATE_IDLE);
        return 0;
    }

    PROV_LOGI("[prov] stop requested");
    mybot_wifi_runtime_request_stop(&ctx->wifi);
    bool callbacks_detached = mybot_wifi_runtime_unregister_callbacks(&ctx->wifi);
    if (ctx->worker_started) {
        rtos_thread_join(&ctx->worker_thread);
        ctx->worker_started = false;
    }
    bool wifi_released = stop_scan(ctx);
    wifi_released = stop_softap(ctx) && wifi_released;
    wifi_released = stop_sta(ctx) && wifi_released;
    if (!wifi_released) {
        PROV_LOGE("[prov] Wi-Fi ownership release incomplete; retaining context");
        return -1;
    }
    if (!callbacks_detached) {
        PROV_LOGE("event callbacks remain registered; retaining provisioning context");
        return -1;
    }

    s_active_ctx = NULL;
    set_provisioning_state(MYBOT_PROVISIONING_STATE_IDLE);
    destroy_context_resources(ctx);
    PROV_LOGI("[prov] stopped");
    return 0;
}

mybot_provisioning_state_t mybot_provisioning_get_state(void) {
    mybot_provisioning_state_t state = MYBOT_PROVISIONING_STATE_IDLE;

    if (state_lock_acquire()) {
        state = s_provisioning_state;
        state_lock_release();
    }
    return state;
}
