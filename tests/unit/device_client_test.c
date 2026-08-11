/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_device_client.h"
#include <mybot/mybot_build_config.h>
#include "mybot_http_client.h"
#include "mybot_json.h"

#include <api/aosl.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char s_request_body[2048];
static char s_request_url[MYBOT_DEVICE_CLIENT_MAX_URL];
static char s_request_headers[MYBOT_DEVICE_CLIENT_MAX_TOKEN + 32];
static const char *s_response_body;
static int s_get_ex_call_count;
static int s_post_ex_call_count;

static const char s_valid_pair_response_body[] =
    "{\"data\":{\"code\":\"123456\",\"pair_token\":\"pair-token\"}}";

static const char s_missing_pair_code_response_body[] =
    "{\"data\":{\"pair_token\":\"pair-token\"}}";

static const char s_missing_pair_token_response_body[] = "{\"data\":{\"code\":\"123456\"}}";

static const char s_valid_response_body[] =
    "{\"data\":{\"conversation_id\":\"conversation-1\",\"rtc\":{"
    "\"app_id\":\"app-1\",\"channel\":\"channel-1\",\"token\":\"token-1\","
    "\"uid\":\"device-uid\"}}}";

static const char s_missing_id_response_body[] =
    "{\"data\":{\"rtc\":{\"app_id\":\"app-1\",\"channel\":\"channel-1\","
    "\"token\":\"token-1\",\"uid\":\"device-uid\"}}}";

static const char s_empty_id_response_body[] =
    "{\"data\":{\"conversation_id\":\"\",\"rtc\":{\"app_id\":\"app-1\","
    "\"channel\":\"channel-1\",\"token\":\"token-1\",\"uid\":\"device-uid\"}}}";

static int mock_http_response(mybot_http_client_response_t *resp) {
    assert(s_response_body != NULL);
    size_t response_len = strlen(s_response_body);
    resp->body = malloc(response_len + 1);
    if (!resp->body) {
        return -1;
    }
    memcpy(resp->body, s_response_body, response_len + 1);
    resp->body_len = response_len;
    resp->status_code = 200;
    return 0;
}

int mybot_http_client_get(const char *url, mybot_http_client_response_t *resp) {
    (void)url;
    (void)resp;
    return -1;
}

int mybot_http_client_post(const char *url, const char *content_type, const char *body,
                           mybot_http_client_response_t *resp) {
    (void)url;
    (void)content_type;
    (void)body;
    return mock_http_response(resp);
}

int mybot_http_client_get_ex(const char *url, const char *extra_headers,
                             mybot_http_client_response_t *resp) {
    (void)url;
    (void)extra_headers;
    (void)resp;
    s_get_ex_call_count++;
    return -1;
}

int mybot_http_client_post_ex(const char *url, const char *content_type, const char *body,
                              const char *extra_headers, mybot_http_client_response_t *resp) {
    (void)content_type;
    s_post_ex_call_count++;
    assert(snprintf(s_request_url, sizeof(s_request_url), "%s", url) < (int)sizeof(s_request_url));
    assert(snprintf(s_request_headers, sizeof(s_request_headers), "%s",
                    extra_headers ? extra_headers : "") < (int)sizeof(s_request_headers));

    int written = snprintf(s_request_body, sizeof(s_request_body), "%s", body);
    if (written < 0 || (size_t)written >= sizeof(s_request_body)) {
        return -1;
    }

    return mock_http_response(resp);
}

void mybot_http_client_response_free(mybot_http_client_response_t *resp) {
    if (!resp) {
        return;
    }
    free(resp->body);
    memset(resp, 0, sizeof(*resp));
}

int main(void) {
    aosl_ctor();

    mybot_device_pair_code_t pair_code;
    s_response_body = s_valid_pair_response_body;
    assert(mybot_device_client_create_pair_code("http://server", "device-1", NULL, NULL,
                                                &pair_code) == 0);
    assert(strcmp(pair_code.code, "123456") == 0);
    assert(strcmp(pair_code.pair_token, "pair-token") == 0);

    s_response_body = s_missing_pair_code_response_body;
    assert(mybot_device_client_create_pair_code("http://server", "device-1", NULL, NULL,
                                                &pair_code) < 0);
    s_response_body = s_missing_pair_token_response_body;
    assert(mybot_device_client_create_pair_code("http://server", "device-1", NULL, NULL,
                                                &pair_code) < 0);

    mybot_device_conversation_t conversation;
    char long_id[MYBOT_DEVICE_CLIENT_MAX_ID + 1];
    char long_id_response_body[1024];
    s_response_body = s_valid_response_body;
    assert(mybot_device_client_start_conversation("http://server", "device-1", "token", NULL,
                                                  &conversation) == 0);
    assert(strcmp(conversation.conversation_id, "conversation-1") == 0);
    assert(strcmp(s_request_url, "http://server/devices/device-1/conversations/start") == 0);
    assert(strcmp(s_request_headers, "Authorization: Device token\r\n") == 0);

    assert(mybot_device_client_start_conversation("http://server", "dev/../x?y#z\r\n", "token",
                                                  NULL, &conversation) == 0);
    assert(strcmp(s_request_url,
                  "http://server/devices/dev%2F..%2Fx%3Fy%23z%0D%0A/conversations/start") == 0);

    char max_device_id[MYBOT_DEVICE_CLIENT_MAX_ID];
    memset(max_device_id, '/', sizeof(max_device_id) - 1);
    max_device_id[sizeof(max_device_id) - 1] = '\0';
    assert(mybot_device_client_start_conversation("http://server", max_device_id, "token", NULL,
                                                  &conversation) == 0);
    assert(strstr(s_request_url, "/devices/%2F%2F%2F") != NULL);
    assert(strlen(s_request_url) < MYBOT_DEVICE_CLIENT_MAX_URL);

    int post_calls = s_post_ex_call_count;
    assert(mybot_device_client_start_conversation(
               "http://server", "device-1", "token\r\nX-Injected: yes", NULL, &conversation) < 0);
    assert(s_post_ex_call_count == post_calls);

    mybot_device_binding_t binding;
    assert(mybot_device_client_get_binding_status("http://server", "device-1",
                                                  "Pair token\nX-Injected: yes", &binding) < 0);
    assert(s_get_ex_call_count == 0);

    s_response_body = s_missing_id_response_body;
    assert(mybot_device_client_start_conversation("http://server", "device-1", "token", NULL,
                                                  &conversation) < 0);
    s_response_body = s_empty_id_response_body;
    assert(mybot_device_client_start_conversation("http://server", "device-1", "token", NULL,
                                                  &conversation) < 0);

    memset(long_id, 'x', sizeof(long_id) - 1);
    long_id[sizeof(long_id) - 1] = '\0';
    assert(snprintf(long_id_response_body, sizeof(long_id_response_body),
                    "{\"data\":{\"conversation_id\":\"%s\",\"rtc\":{"
                    "\"app_id\":\"app-1\",\"channel\":\"channel-1\","
                    "\"token\":\"token-1\",\"uid\":\"device-uid\"}}}",
                    long_id) < (int)sizeof(long_id_response_body));
    s_response_body = long_id_response_body;
    assert(mybot_device_client_start_conversation("http://server", "device-1", "token", NULL,
                                                  &conversation) < 0);

    mybot_json_t *root = mybot_json_parse(s_request_body);
    assert(root != NULL);
    mybot_json_t *audio = mybot_json_get_object_item(root, "audio");
    assert(audio != NULL);

    int64_t ptime = 0;
    assert(mybot_json_get_integer(mybot_json_get_object_item(audio, "p_time"), &ptime));
    assert(ptime == MYBOT_AUDIO_PTIME_MS);
    assert(strcmp(mybot_json_get_string(mybot_json_get_object_item(audio, "codec")), "G722") == 0);

    mybot_json_delete(root);
    aosl_dtor();
    puts("device_client_test: ok");
    return 0;
}
