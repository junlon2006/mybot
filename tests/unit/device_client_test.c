/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_device_client.h"
#include <mybot/mybot_build_config.h>
#include "mybot_http_client.h"
#include "mybot_json.h"

#include <api/aosl.h>

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char s_request_body[2048];
static char s_request_url[MYBOT_DEVICE_CLIENT_MAX_URL];
static char s_request_headers[MYBOT_DEVICE_CLIENT_MAX_TOKEN + 32];
static char s_request_content_type[64];
static const char *s_response_body;
static int s_http_result;
static int s_response_status = 200;
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

static const char s_valid_renew_response_body[] =
    "{\"data\":{\"rtc\":{\"app_id\":\"app-1\",\"channel\":\"channel-1\","
    "\"token\":\"renewed-token\",\"uid\":\"device-uid\"}}}";

static const char s_missing_renew_token_response_body[] =
    "{\"data\":{\"rtc\":{\"app_id\":\"app-1\",\"channel\":\"channel-1\","
    "\"uid\":\"device-uid\"}}}";

static const char s_large_binding_poll_response_body[] =
    "{\"data\":{\"status\":\"bound\",\"poll_after_seconds\":9223372036854775807}}";

static void reset_http_mock(const char *body) {
    s_response_body = body;
    s_http_result = 0;
    s_response_status = 200;
}

static void capture_request(const char *url, const char *content_type, const char *body,
                            const char *headers) {
    assert(snprintf(s_request_url, sizeof(s_request_url), "%s", url ? url : "") <
           (int)sizeof(s_request_url));
    assert(snprintf(s_request_content_type, sizeof(s_request_content_type), "%s",
                    content_type ? content_type : "") < (int)sizeof(s_request_content_type));
    assert(snprintf(s_request_headers, sizeof(s_request_headers), "%s", headers ? headers : "") <
           (int)sizeof(s_request_headers));
    assert(snprintf(s_request_body, sizeof(s_request_body), "%s", body ? body : "") <
           (int)sizeof(s_request_body));
}

static int mock_http_response(mybot_http_client_response_t *resp) {
    if (!resp) {
        return -1;
    }
    resp->status_code = s_response_status;
    if (s_response_body) {
        size_t response_len = strlen(s_response_body);
        resp->body = malloc(response_len + 1);
        if (!resp->body) {
            return -1;
        }
        memcpy(resp->body, s_response_body, response_len + 1);
        resp->body_len = response_len;
    }
    return 0;
}

int mybot_http_client_get(const char *url, mybot_http_client_response_t *resp) {
    (void)url;
    (void)resp;
    return -1;
}

int mybot_http_client_post(const char *url, const char *content_type, const char *body,
                           mybot_http_client_response_t *resp) {
    capture_request(url, content_type, body, NULL);
    if (s_http_result < 0) {
        return s_http_result;
    }
    return mock_http_response(resp);
}

int mybot_http_client_get_ex(const char *url, const char *extra_headers,
                             mybot_http_client_response_t *resp) {
    s_get_ex_call_count++;
    capture_request(url, NULL, NULL, extra_headers);
    if (s_http_result < 0) {
        return s_http_result;
    }
    return mock_http_response(resp);
}

int mybot_http_client_post_ex(const char *url, const char *content_type, const char *body,
                              const char *extra_headers, mybot_http_client_response_t *resp) {
    s_post_ex_call_count++;
    capture_request(url, content_type, body, extra_headers);
    if (s_http_result < 0) {
        return s_http_result;
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

static void test_required_arguments(void) {
    mybot_device_pair_code_t pair;
    mybot_device_binding_t binding;
    mybot_device_conversation_t conversation;
    mybot_device_rtc_token_t token;

    assert(mybot_device_client_create_pair_code(NULL, "device", NULL, NULL, &pair) < 0);
    assert(mybot_device_client_create_pair_code("http://server", NULL, NULL, NULL, &pair) < 0);
    assert(mybot_device_client_create_pair_code("http://server", "device", NULL, NULL, NULL) < 0);

    assert(mybot_device_client_get_binding_status(NULL, "device", "Pair token", &binding) < 0);
    assert(mybot_device_client_get_binding_status("http://server", NULL, "Pair token", &binding) <
           0);
    assert(mybot_device_client_get_binding_status("http://server", "device", NULL, &binding) < 0);
    assert(mybot_device_client_get_binding_status("http://server", "device", "Pair token", NULL) <
           0);

    assert(mybot_device_client_start_conversation(NULL, "device", "token", "{}", &conversation) <
           0);
    assert(mybot_device_client_start_conversation("http://server", NULL, "token", "{}",
                                                  &conversation) < 0);
    assert(mybot_device_client_start_conversation("http://server", "device", NULL, "{}",
                                                  &conversation) < 0);
    assert(mybot_device_client_start_conversation("http://server", "device", "token", "{}", NULL) <
           0);

    assert(mybot_device_client_renew_rtc_token(NULL, "device", "token", "channel", "uid", &token) <
           0);
    assert(mybot_device_client_renew_rtc_token("http://server", NULL, "token", "channel", "uid",
                                               &token) < 0);
    assert(mybot_device_client_renew_rtc_token("http://server", "device", NULL, "channel", "uid",
                                               &token) < 0);
    assert(mybot_device_client_renew_rtc_token("http://server", "device", "token", NULL, "uid",
                                               &token) < 0);
    assert(mybot_device_client_renew_rtc_token("http://server", "device", "token", "", "uid",
                                               &token) < 0);
    assert(mybot_device_client_renew_rtc_token("http://server", "device", "token", "channel", NULL,
                                               &token) < 0);
    assert(mybot_device_client_renew_rtc_token("http://server", "device", "token", "channel", "",
                                               &token) < 0);
    assert(mybot_device_client_renew_rtc_token("http://server", "device", "token", "channel", "uid",
                                               NULL) < 0);

    assert(mybot_device_client_stop_conversation(NULL, "device", "token", "conversation", NULL) <
           0);
    assert(mybot_device_client_stop_conversation("http://server", NULL, "token", "conversation",
                                                 NULL) < 0);
    assert(mybot_device_client_stop_conversation("http://server", "device", NULL, "conversation",
                                                 NULL) < 0);
    assert(mybot_device_client_stop_conversation("http://server", "device", "token", NULL, NULL) <
           0);
}

static void test_pair_code_failures(void) {
    mybot_device_pair_code_t pair;
    mybot_json_t *request;

    reset_http_mock("{\"data\":{\"device_id\":\"device-2\",\"code\":\"654321\","
                    "\"pair_token\":\"pair-2\",\"expires_in_seconds\":30,"
                    "\"poll_after_seconds\":4}}");
    assert(mybot_device_client_create_pair_code("http://server", "device-2", "1.2.3", "hw-a",
                                                &pair) == 0);
    assert(strcmp(s_request_url, "http://server/devices/pair-codes") == 0);
    assert(strcmp(s_request_content_type, "application/json") == 0);
    request = mybot_json_parse(s_request_body);
    assert(request != NULL);
    assert(strcmp(mybot_json_get_string(mybot_json_get_object_item(request, "device_id")),
                  "device-2") == 0);
    assert(strcmp(mybot_json_get_string(mybot_json_get_object_item(request, "firmware_version")),
                  "1.2.3") == 0);
    assert(strcmp(mybot_json_get_string(mybot_json_get_object_item(request, "hardware_model")),
                  "hw-a") == 0);
    mybot_json_delete(request);
    assert(strcmp(pair.device_id, "device-2") == 0);
    assert(pair.expires_in_seconds == 30);
    assert(pair.poll_after_seconds == 4);

    reset_http_mock(s_valid_pair_response_body);
    s_http_result = -1;
    assert(mybot_device_client_create_pair_code("http://server", "device", "", "", &pair) < 0);

    reset_http_mock(s_valid_pair_response_body);
    s_response_status = 503;
    assert(mybot_device_client_create_pair_code("http://server", "device", NULL, NULL, &pair) ==
           503);
    reset_http_mock(s_valid_pair_response_body);
    s_response_status = 0;
    assert(mybot_device_client_create_pair_code("http://server", "device", NULL, NULL, &pair) < 0);

    reset_http_mock(NULL);
    assert(mybot_device_client_create_pair_code("http://server", "device", NULL, NULL, &pair) < 0);
    reset_http_mock("not-json");
    assert(mybot_device_client_create_pair_code("http://server", "device", NULL, NULL, &pair) < 0);
    reset_http_mock("{}");
    assert(mybot_device_client_create_pair_code("http://server", "device", NULL, NULL, &pair) < 0);
    reset_http_mock("{\"data\":{\"code\":\"1234567890123456\",\"pair_token\":\"token\"}}");
    assert(mybot_device_client_create_pair_code("http://server", "device", NULL, NULL, &pair) < 0);
    reset_http_mock("{\"data\":{\"code\":\"123456\",\"pair_token\":\"\"}}");
    assert(mybot_device_client_create_pair_code("http://server", "device", NULL, NULL, &pair) < 0);
}

static void test_binding_failures(void) {
    mybot_device_binding_t binding;
    char long_value[700];
    int calls;

    reset_http_mock("{\"data\":{\"status\":\"bound\",\"device_token\":\"device-token\","
                    "\"agent_id\":\"agent-1\",\"agent_name\":\"Agent\","
                    "\"poll_after_seconds\":7}}");
    assert(mybot_device_client_get_binding_status("http://server", "device/one", "Pair pair-token",
                                                  &binding) == 0);
    assert(strcmp(s_request_url, "http://server/devices/device%2Fone/binding-status") == 0);
    assert(strcmp(s_request_headers, "Authorization: Pair pair-token\r\n") == 0);
    assert(strcmp(binding.device_token, "device-token") == 0);
    assert(strcmp(binding.agent_id, "agent-1") == 0);
    assert(strcmp(binding.agent_name, "Agent") == 0);
    assert(binding.poll_after_seconds == 7);

    calls = s_get_ex_call_count;
    assert(mybot_device_client_get_binding_status("http://server", "", "Pair token", &binding) < 0);
    memset(long_value, 'b', sizeof(long_value) - 1);
    long_value[sizeof(long_value) - 1] = '\0';
    assert(mybot_device_client_get_binding_status(long_value, "device", "Pair token", &binding) <
           0);
    assert(mybot_device_client_get_binding_status("http://server", "device", "", &binding) < 0);
    assert(mybot_device_client_get_binding_status("http://server", "device", "Pair\x7ftoken",
                                                  &binding) < 0);
    assert(mybot_device_client_get_binding_status("http://server", "device", long_value, &binding) <
           0);
    assert(s_get_ex_call_count == calls);

    reset_http_mock("{}");
    s_http_result = -1;
    assert(mybot_device_client_get_binding_status("http://server", "device", "Pair token",
                                                  &binding) < 0);
    reset_http_mock("{}");
    s_response_status = 404;
    assert(mybot_device_client_get_binding_status("http://server", "device", "Pair token",
                                                  &binding) == 404);
    reset_http_mock("{}");
    s_response_status = 0;
    assert(mybot_device_client_get_binding_status("http://server", "device", "Pair token",
                                                  &binding) < 0);
    reset_http_mock(NULL);
    assert(mybot_device_client_get_binding_status("http://server", "device", "Pair token",
                                                  &binding) < 0);
    reset_http_mock("bad-json");
    assert(mybot_device_client_get_binding_status("http://server", "device", "Pair token",
                                                  &binding) < 0);
    reset_http_mock("{}");
    assert(mybot_device_client_get_binding_status("http://server", "device", "Pair token",
                                                  &binding) < 0);
}

static void test_conversation_failures(void) {
    mybot_device_conversation_t conversation;
    char long_base[700];
    int calls;

    reset_http_mock(s_valid_response_body);
    assert(mybot_device_client_start_conversation("http://server", "device", "token",
                                                  "{\"custom\":true}", &conversation) == 0);
    assert(strcmp(s_request_body, "{\"custom\":true}") == 0);
    assert(strcmp(s_request_content_type, "application/json") == 0);

    calls = s_post_ex_call_count;
    assert(mybot_device_client_start_conversation("http://server", "", "token", "{}",
                                                  &conversation) < 0);
    assert(mybot_device_client_start_conversation("http://server", "device", "", "{}",
                                                  &conversation) < 0);
    memset(long_base, 'u', sizeof(long_base) - 1);
    long_base[sizeof(long_base) - 1] = '\0';
    assert(mybot_device_client_start_conversation(long_base, "device", "token", "{}",
                                                  &conversation) < 0);
    assert(s_post_ex_call_count == calls);

    reset_http_mock(s_valid_response_body);
    s_http_result = -1;
    assert(mybot_device_client_start_conversation("http://server", "device", "token", "{}",
                                                  &conversation) < 0);
    reset_http_mock(s_valid_response_body);
    s_response_status = 401;
    assert(mybot_device_client_start_conversation("http://server", "device", "token", "{}",
                                                  &conversation) == 401);
    reset_http_mock(s_valid_response_body);
    s_response_status = 0;
    assert(mybot_device_client_start_conversation("http://server", "device", "token", "{}",
                                                  &conversation) < 0);
    reset_http_mock(NULL);
    assert(mybot_device_client_start_conversation("http://server", "device", "token", "{}",
                                                  &conversation) < 0);
    reset_http_mock("invalid-json");
    assert(mybot_device_client_start_conversation("http://server", "device", "token", "{}",
                                                  &conversation) < 0);
    reset_http_mock("{}");
    assert(mybot_device_client_start_conversation("http://server", "device", "token", "{}",
                                                  &conversation) < 0);
    reset_http_mock("{\"data\":{\"conversation_id\":\"c-1\"}}");
    assert(mybot_device_client_start_conversation("http://server", "device", "token", "{}",
                                                  &conversation) < 0);
    reset_http_mock("{\"data\":{\"conversation_id\":\"c-1\","
                    "\"rtc\":{\"channel\":\"\",\"uid\":\"uid\"}}}");
    assert(mybot_device_client_start_conversation("http://server", "device", "token", "{}",
                                                  &conversation) < 0);
    reset_http_mock("{\"data\":{\"conversation_id\":\"c-1\","
                    "\"rtc\":{\"channel\":\"channel\"}}}");
    assert(mybot_device_client_start_conversation("http://server", "device", "token", "{}",
                                                  &conversation) < 0);
    reset_http_mock("{\"data\":{\"conversation_id\":\"c-1\","
                    "\"rtc\":{\"channel\":\"channel\",\"uid\":\"uid\"}}}");
    assert(mybot_device_client_start_conversation("http://server", "device", "token", "{}",
                                                  &conversation) == 0);
    assert(strcmp(conversation.rtc_channel, "channel") == 0);
    assert(strcmp(conversation.rtc_uid, "uid") == 0);
    assert(conversation.rtc_token[0] == '\0');
}

static void test_renew_failures(void) {
    mybot_device_rtc_token_t token;
    char long_base[700];
    int calls;

    calls = s_post_ex_call_count;
    assert(mybot_device_client_renew_rtc_token("http://server", "", "token", "channel", "uid",
                                               &token) < 0);
    assert(mybot_device_client_renew_rtc_token("http://server", "device", "", "channel", "uid",
                                               &token) < 0);
    memset(long_base, 'u', sizeof(long_base) - 1);
    long_base[sizeof(long_base) - 1] = '\0';
    assert(mybot_device_client_renew_rtc_token(long_base, "device", "token", "channel", "uid",
                                               &token) < 0);
    assert(s_post_ex_call_count == calls);

    reset_http_mock(s_valid_renew_response_body);
    s_http_result = -1;
    assert(mybot_device_client_renew_rtc_token("http://server", "device", "token", "channel", "uid",
                                               &token) < 0);
    reset_http_mock(s_valid_renew_response_body);
    s_response_status = 403;
    assert(mybot_device_client_renew_rtc_token("http://server", "device", "token", "channel", "uid",
                                               &token) == 403);
    reset_http_mock(s_valid_renew_response_body);
    s_response_status = 0;
    assert(mybot_device_client_renew_rtc_token("http://server", "device", "token", "channel", "uid",
                                               &token) < 0);
    reset_http_mock(NULL);
    assert(mybot_device_client_renew_rtc_token("http://server", "device", "token", "channel", "uid",
                                               &token) < 0);
    reset_http_mock("invalid-json");
    assert(mybot_device_client_renew_rtc_token("http://server", "device", "token", "channel", "uid",
                                               &token) < 0);
    reset_http_mock("{}");
    assert(mybot_device_client_renew_rtc_token("http://server", "device", "token", "channel", "uid",
                                               &token) < 0);
    reset_http_mock("{\"data\":{}}");
    assert(mybot_device_client_renew_rtc_token("http://server", "device", "token", "channel", "uid",
                                               &token) < 0);
    reset_http_mock("{\"data\":{\"rtc\":{\"uid\":\"uid\",\"token\":\"token\"}}}");
    assert(mybot_device_client_renew_rtc_token("http://server", "device", "token", "channel", "uid",
                                               &token) < 0);
    reset_http_mock("{\"data\":{\"rtc\":{\"channel\":\"channel\",\"token\":\"token\"}}}");
    assert(mybot_device_client_renew_rtc_token("http://server", "device", "token", "channel", "uid",
                                               &token) < 0);
    reset_http_mock("{\"data\":{\"rtc\":{\"channel\":\"channel\",\"uid\":\"uid\","
                    "\"token\":\"renewed\"}}}");
    assert(mybot_device_client_renew_rtc_token("http://server", "device", "token", "channel", "uid",
                                               &token) == 0);
    assert(token.rtc_app_id[0] == '\0');
    assert(strcmp(token.rtc_token, "renewed") == 0);
}

static void test_stop_conversation(void) {
    mybot_json_t *request;
    char long_base[700];
    int calls;

    reset_http_mock(NULL);
    assert(mybot_device_client_stop_conversation("http://server", "device/one", "token", "c-1",
                                                 "device_hangup") == 0);
    assert(strcmp(s_request_url, "http://server/devices/device%2Fone/conversations/stop") == 0);
    assert(strcmp(s_request_headers, "Authorization: Device token\r\n") == 0);
    request = mybot_json_parse(s_request_body);
    assert(request != NULL);
    assert(strcmp(mybot_json_get_string(mybot_json_get_object_item(request, "conversation_id")),
                  "c-1") == 0);
    assert(strcmp(mybot_json_get_string(mybot_json_get_object_item(request, "reason")),
                  "device_hangup") == 0);
    mybot_json_delete(request);

    reset_http_mock(NULL);
    assert(mybot_device_client_stop_conversation("http://server", "device", "token", "c-2", NULL) ==
           0);
    request = mybot_json_parse(s_request_body);
    assert(request != NULL);
    assert(mybot_json_get_object_item(request, "reason") == NULL);
    mybot_json_delete(request);

    calls = s_post_ex_call_count;
    assert(mybot_device_client_stop_conversation("http://server", "", "token", "c-1", NULL) < 0);
    assert(mybot_device_client_stop_conversation("http://server", "device", "", "c-1", NULL) < 0);
    memset(long_base, 'u', sizeof(long_base) - 1);
    long_base[sizeof(long_base) - 1] = '\0';
    assert(mybot_device_client_stop_conversation(long_base, "device", "token", "c-1", NULL) < 0);
    assert(s_post_ex_call_count == calls);

    reset_http_mock(NULL);
    s_http_result = -1;
    assert(mybot_device_client_stop_conversation("http://server", "device", "token", "c-1", NULL) <
           0);
    reset_http_mock(NULL);
    s_response_status = 409;
    assert(mybot_device_client_stop_conversation("http://server", "device", "token", "c-1", NULL) ==
           409);
    reset_http_mock(NULL);
    s_response_status = 0;
    assert(mybot_device_client_stop_conversation("http://server", "device", "token", "c-1", NULL) <
           0);
}

int main(void) {
    aosl_ctor();
    test_required_arguments();

    mybot_device_pair_code_t pair_code;
    reset_http_mock(s_valid_pair_response_body);
    assert(mybot_device_client_create_pair_code("http://server", "device-1", NULL, NULL,
                                                &pair_code) == 0);
    assert(strcmp(pair_code.code, "123456") == 0);
    assert(strcmp(pair_code.pair_token, "pair-token") == 0);

    reset_http_mock(s_missing_pair_code_response_body);
    assert(mybot_device_client_create_pair_code("http://server", "device-1", NULL, NULL,
                                                &pair_code) < 0);
    reset_http_mock(s_missing_pair_token_response_body);
    assert(mybot_device_client_create_pair_code("http://server", "device-1", NULL, NULL,
                                                &pair_code) < 0);

    mybot_device_conversation_t conversation;
    char long_id[MYBOT_DEVICE_CLIENT_MAX_ID + 1];
    char long_id_response_body[1024];
    reset_http_mock(s_valid_response_body);
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

    mybot_device_rtc_token_t renewed;
    reset_http_mock(s_valid_renew_response_body);
    assert(mybot_device_client_renew_rtc_token("http://server", "device-1", "token", "channel-1",
                                               "device-uid", &renewed) == 0);
    assert(strcmp(renewed.rtc_app_id, "app-1") == 0);
    assert(strcmp(renewed.rtc_channel, "channel-1") == 0);
    assert(strcmp(renewed.rtc_uid, "device-uid") == 0);
    assert(strcmp(renewed.rtc_token, "renewed-token") == 0);
    assert(strcmp(s_request_url, "http://server/devices/device-1/rtc-token") == 0);
    assert(strcmp(s_request_headers, "Authorization: Device token\r\n") == 0);
    {
        mybot_json_t *renew_body = mybot_json_parse(s_request_body);
        assert(renew_body != NULL);
        assert(strcmp(mybot_json_get_string(mybot_json_get_object_item(renew_body, "channel")),
                      "channel-1") == 0);
        assert(strcmp(mybot_json_get_string(mybot_json_get_object_item(renew_body, "local_uid")),
                      "device-uid") == 0);
        mybot_json_delete(renew_body);
    }

    reset_http_mock(s_missing_renew_token_response_body);
    assert(mybot_device_client_renew_rtc_token("http://server", "device-1", "token", "channel-1",
                                               "device-uid", &renewed) < 0);
    post_calls = s_post_ex_call_count;
    assert(mybot_device_client_renew_rtc_token("http://server", "device-1", "token\nInjected",
                                               "channel-1", "device-uid", &renewed) < 0);
    assert(s_post_ex_call_count == post_calls);

    mybot_device_binding_t binding;
    assert(mybot_device_client_get_binding_status("http://server", "device-1",
                                                  "Pair token\nX-Injected: yes", &binding) < 0);
    assert(s_get_ex_call_count == 0);

    reset_http_mock(s_large_binding_poll_response_body);
    assert(mybot_device_client_get_binding_status("http://server", "device-1", "Pair token",
                                                  &binding) == 0);
    assert(binding.poll_after_seconds == INT_MAX);

    reset_http_mock(s_missing_id_response_body);
    assert(mybot_device_client_start_conversation("http://server", "device-1", "token", NULL,
                                                  &conversation) < 0);
    reset_http_mock(s_empty_id_response_body);
    assert(mybot_device_client_start_conversation("http://server", "device-1", "token", NULL,
                                                  &conversation) < 0);

    memset(long_id, 'x', sizeof(long_id) - 1);
    long_id[sizeof(long_id) - 1] = '\0';
    assert(snprintf(long_id_response_body, sizeof(long_id_response_body),
                    "{\"data\":{\"conversation_id\":\"%s\",\"rtc\":{"
                    "\"app_id\":\"app-1\",\"channel\":\"channel-1\","
                    "\"token\":\"token-1\",\"uid\":\"device-uid\"}}}",
                    long_id) < (int)sizeof(long_id_response_body));
    reset_http_mock(long_id_response_body);
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
    test_pair_code_failures();
    test_binding_failures();
    test_conversation_failures();
    test_renew_failures();
    test_stop_conversation();
    aosl_dtor();
    puts("device_client_test: ok");
    return 0;
}
