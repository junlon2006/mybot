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
static int s_response_body_alloc_count;
static int s_response_body_free_count;
static size_t s_parse_fail_after_http;
static size_t s_json_alloc_count;
static size_t s_json_fail_at;
static int s_json_alloc_failures;
static int s_json_live_allocations;

#ifdef MYBOT_TEST_WRAP_DEVICE_HEADERS
static bool s_track_header;
static bool s_fail_header;
static void *s_header_allocation;
static int s_header_alloc_calls;
static int s_header_free_calls;
static const char *s_borrowed_body;

void *__real_aosl_hal_malloc(size_t size);
void __real_aosl_hal_free(void *ptr);

void *__wrap_aosl_hal_malloc(size_t size) {
    bool header = s_track_header && size == MYBOT_DEVICE_CLIENT_MAX_TOKEN + 32U;
    if (header) {
        assert(s_header_allocation == NULL);
        s_header_alloc_calls++;
        if (s_fail_header) {
            return NULL;
        }
    }
    void *ptr = __real_aosl_hal_malloc(size);
    if (header) {
        s_header_allocation = ptr;
    }
    return ptr;
}

void __wrap_aosl_hal_free(void *ptr) {
    assert(!s_borrowed_body || ptr != s_borrowed_body);
    if (s_track_header && ptr && ptr == s_header_allocation) {
        s_header_free_calls++;
        s_header_allocation = NULL;
    }
    __real_aosl_hal_free(ptr);
}
#endif

static const char s_valid_pair_response_body[] =
    "{\"data\":{\"code\":\"123456\",\"pair_token\":\"pair-token\"}}";

static const char s_missing_pair_code_response_body[] =
    "{\"data\":{\"pair_token\":\"pair-token\"}}";

static const char s_missing_pair_token_response_body[] = "{\"data\":{\"code\":\"123456\"}}";

static const char s_valid_response_body[] =
    "{\"data\":{\"conversation_id\":\"conversation-1\",\"agent_uid\":\"agent-uid\",\"rtc\":{"
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
        s_response_body_alloc_count++;
    }
    if (s_parse_fail_after_http) {
        s_json_fail_at = s_json_alloc_count + s_parse_fail_after_http;
    }
    return 0;
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
#ifdef MYBOT_TEST_WRAP_DEVICE_HEADERS
    if (s_track_header) {
        assert(extra_headers == s_header_allocation);
        if (s_borrowed_body) {
            assert(body == s_borrowed_body);
        }
    }
#endif
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
    if (resp->body) {
        s_response_body_free_count++;
        free(resp->body);
    }
    memset(resp, 0, sizeof(*resp));
}

static void expect_output_bytes(const void *output, size_t size, unsigned char value) {
    const unsigned char *bytes = output;
    for (size_t i = 0; i < size; ++i) {
        assert(bytes[i] == value);
    }
}

static void test_required_arguments(void) {
    mybot_device_pair_code_t pair;
    mybot_device_binding_t binding;
    mybot_device_conversation_t conversation;
    mybot_device_rtc_token_t token;
    memset(&pair, 0x5a, sizeof(pair));
    memset(&binding, 0x5a, sizeof(binding));
    memset(&conversation, 0x5a, sizeof(conversation));
    memset(&token, 0x5a, sizeof(token));

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
    expect_output_bytes(&pair, sizeof(pair), 0x5a);
    expect_output_bytes(&binding, sizeof(binding), 0x5a);
    expect_output_bytes(&conversation, sizeof(conversation), 0x5a);
    expect_output_bytes(&token, sizeof(token), 0x5a);
}

typedef enum { PARSED_PAIR, PARSED_BINDING, PARSED_START, PARSED_RENEW } parsed_call_t;

typedef union {
    mybot_device_pair_code_t pair;
    mybot_device_binding_t binding;
    mybot_device_conversation_t conversation;
    mybot_device_rtc_token_t token;
} parsed_output_t;

static const size_t s_parsed_output_size[] = {
    sizeof(mybot_device_pair_code_t),
    sizeof(mybot_device_binding_t),
    sizeof(mybot_device_conversation_t),
    sizeof(mybot_device_rtc_token_t),
};

static int call_parsed_response(parsed_call_t operation, parsed_output_t *output) {
    switch (operation) {
    case PARSED_PAIR:
        return mybot_device_client_create_pair_code("http://server", "device", NULL, NULL,
                                                    &output->pair);
    case PARSED_BINDING:
        return mybot_device_client_get_binding_status("http://server", "device", "Device token",
                                                      &output->binding);
    case PARSED_START:
        return mybot_device_client_start_conversation("http://server", "device", "token", NULL,
                                                      &output->conversation);
    case PARSED_RENEW:
        return mybot_device_client_renew_rtc_token("http://server", "device", "token", "channel-1",
                                                   "device-uid", &output->token);
    }
    return -1;
}

static void test_outer_data_contract(void) {
    const char *responses[] = {"{}", "{\"data\":null}", "{\"data\":123}", "{\"data\":[]}",
                               "{\"data\":{}}"};
    for (int operation = PARSED_PAIR; operation <= PARSED_RENEW; ++operation) {
        for (size_t i = 0; i < sizeof(responses) / sizeof(responses[0]); ++i) {
            reset_http_mock(responses[i]);
            int allocations_before = s_response_body_alloc_count;
            int frees_before = s_response_body_free_count;
            parsed_output_t output;
            memset(&output, 0x5a, sizeof(output));
            int ret = call_parsed_response((parsed_call_t)operation, &output);
            assert(ret == (operation == PARSED_BINDING && i != 0 ? 0 : -1));
            expect_output_bytes(&output, s_parsed_output_size[operation], 0);
            assert(s_response_body_alloc_count == allocations_before + 1);
            assert(s_response_body_free_count == frees_before + 1);
        }
    }
}

static void test_partial_parsed_outputs(void) {
    int allocations_before = s_response_body_alloc_count;
    int frees_before = s_response_body_free_count;
    parsed_output_t output;
    reset_http_mock(s_missing_pair_token_response_body);
    assert(call_parsed_response(PARSED_PAIR, &output) == -1);
    assert(strcmp(output.pair.code, "123456") == 0);
    assert(output.pair.pair_token[0] == '\0');
    reset_http_mock(s_missing_pair_code_response_body);
    assert(call_parsed_response(PARSED_PAIR, &output) == -1);
    assert(output.pair.code[0] == '\0');
    assert(strcmp(output.pair.pair_token, "pair-token") == 0);

    reset_http_mock(s_missing_id_response_body);
    assert(call_parsed_response(PARSED_START, &output) == -1);
    expect_output_bytes(&output.conversation, sizeof(output.conversation), 0);
    reset_http_mock("{\"data\":{\"conversation_id\":\"c-1\"}}");
    assert(call_parsed_response(PARSED_START, &output) == -1);
    assert(strcmp(output.conversation.conversation_id, "c-1") == 0);
    assert(output.conversation.rtc_app_id[0] == '\0');
    reset_http_mock(s_missing_renew_token_response_body);
    assert(call_parsed_response(PARSED_RENEW, &output) == -1);
    assert(strcmp(output.token.rtc_channel, "channel-1") == 0);
    assert(strcmp(output.token.rtc_uid, "device-uid") == 0);
    assert(output.token.rtc_token[0] == '\0');
    assert(s_response_body_alloc_count == allocations_before + 5);
    assert(s_response_body_free_count == frees_before + 5);
}

static void *parse_test_malloc(size_t size) {
    s_json_alloc_count++;
    if (s_json_fail_at && s_json_alloc_count == s_json_fail_at) {
        s_json_alloc_failures++;
        return NULL;
    }
    void *ptr = malloc(size);
    if (ptr) {
        s_json_live_allocations++;
    }
    return ptr;
}

static void parse_test_free(void *ptr) {
    if (ptr) {
        assert(s_json_live_allocations > 0);
        s_json_live_allocations--;
        free(ptr);
    }
}

static void test_response_parse_allocation_failure(void) {
    const char *responses[] = {s_valid_pair_response_body,
                               "{\"data\":{\"status\":\"bound\",\"device_token\":\"token\"}}",
                               s_valid_response_body, s_valid_renew_response_body};
    const size_t fail_positions[] = {1, 2, 5};
    const mybot_json_hooks_t hooks = {.malloc_fn = parse_test_malloc, .free_fn = parse_test_free};
    assert(mybot_json_init_hooks(&hooks) == 0);
    for (int operation = PARSED_PAIR; operation <= PARSED_RENEW; ++operation) {
        for (size_t i = 0; i < sizeof(fail_positions) / sizeof(fail_positions[0]); ++i) {
            reset_http_mock(responses[operation]);
            s_json_alloc_count = 0;
            s_json_fail_at = 0;
            s_json_alloc_failures = 0;
            s_parse_fail_after_http = fail_positions[i];
            int allocations_before = s_response_body_alloc_count;
            int frees_before = s_response_body_free_count;
            parsed_output_t output;
            int ret = call_parsed_response((parsed_call_t)operation, &output);
            assert(ret == -1);
            assert(s_json_alloc_failures == 1);
            assert(s_json_live_allocations == 0);
            assert(s_response_body_alloc_count == allocations_before + 1);
            assert(s_response_body_free_count == frees_before + 1);
        }
    }
    s_parse_fail_after_http = 0;
    s_json_fail_at = 0;
    assert(mybot_json_init_hooks(NULL) == 0);
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
    assert(mybot_device_client_get_binding_status("http://server", long_value, "Pair token",
                                                  &binding) < 0);
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
    char oversized_agent_response[512];
    char oversized_rtc_response[512];
    char token[MYBOT_DEVICE_CLIENT_MAX_RTC_TOKEN];
    char oversized_token[MYBOT_DEVICE_CLIENT_MAX_RTC_TOKEN + 1];
    char token_response[2048];
    int calls;

    reset_http_mock(s_valid_response_body);
    assert(mybot_device_client_start_conversation("http://server", "device", "token",
                                                  "{\"custom\":true}", &conversation) == 0);
    assert(strcmp(conversation.rtc_agent_uid, "agent-uid") == 0);
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

    assert(snprintf(oversized_rtc_response, sizeof(oversized_rtc_response),
                    "{\"data\":{\"conversation_id\":\"c-1\",\"rtc\":{"
                    "\"app_id\":\"%064d\",\"channel\":\"channel\",\"uid\":\"uid\"}}}",
                    0) > 0);
    reset_http_mock(oversized_rtc_response);
    assert(mybot_device_client_start_conversation("http://server", "device", "token", "{}",
                                                  &conversation) < 0);
    assert(snprintf(oversized_rtc_response, sizeof(oversized_rtc_response),
                    "{\"data\":{\"conversation_id\":\"c-1\",\"rtc\":{"
                    "\"app_id\":\"app-1\",\"channel\":\"%0128d\",\"uid\":\"uid\"}}}",
                    0) > 0);
    reset_http_mock(oversized_rtc_response);
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
    /* app_id and agent_uid are required identity fields. */
    reset_http_mock("{\"data\":{\"conversation_id\":\"c-1\","
                    "\"rtc\":{\"channel\":\"channel\",\"uid\":\"uid\"}}}");
    assert(mybot_device_client_start_conversation("http://server", "device", "token", "{}",
                                                  &conversation) < 0);
    reset_http_mock("{\"data\":{\"conversation_id\":\"c-1\",\"agent_uid\":\"agent\","
                    "\"rtc\":{\"app_id\":\"app\",\"channel\":\"channel\","
                    "\"uid\":\"uid\"}}}");
    assert(mybot_device_client_start_conversation("http://server", "device", "token", "{}",
                                                  &conversation) == 0);
    assert(strcmp(conversation.rtc_app_id, "app") == 0);
    assert(strcmp(conversation.rtc_channel, "channel") == 0);
    assert(strcmp(conversation.rtc_uid, "uid") == 0);
    assert(conversation.rtc_token[0] == '\0');

    reset_http_mock(s_valid_response_body);
    assert(mybot_device_client_start_conversation("http://server", "device", "token", "{}",
                                                  &conversation) == 0);
    assert(strcmp(conversation.rtc_agent_uid, "agent-uid") == 0);

    /* Agora allows 512 bytes of token content; the destination must retain
     * all bytes plus the terminating NUL and reject the next byte. */
    memset(token, 't', sizeof(token) - 1);
    token[sizeof(token) - 1] = '\0';
    assert(snprintf(token_response, sizeof(token_response),
                    "{\"data\":{\"conversation_id\":\"c-1\",\"agent_uid\":\"agent\","
                    "\"rtc\":{\"app_id\":\"app\",\"channel\":\"channel\","
                    "\"uid\":\"uid\",\"token\":\"%s\"}}}",
                    token) > 0);
    reset_http_mock(token_response);
    assert(mybot_device_client_start_conversation("http://server", "device", "token", "{}",
                                                  &conversation) == 0);
    assert(strlen(conversation.rtc_token) == sizeof(token) - 1);
    memset(oversized_token, 't', sizeof(oversized_token) - 1);
    oversized_token[sizeof(oversized_token) - 1] = '\0';
    assert(snprintf(token_response, sizeof(token_response),
                    "{\"data\":{\"conversation_id\":\"c-1\",\"agent_uid\":\"agent\","
                    "\"rtc\":{\"app_id\":\"app\",\"channel\":\"channel\","
                    "\"uid\":\"uid\",\"token\":\"%s\"}}}",
                    oversized_token) > 0);
    reset_http_mock(token_response);
    assert(mybot_device_client_start_conversation("http://server", "device", "token", "{}",
                                                  &conversation) < 0);

    /* RTM peer accounts must not be silently truncated at the 64-byte limit. */
    assert(snprintf(oversized_agent_response, sizeof(oversized_agent_response),
                    "{\"data\":{\"conversation_id\":\"c-1\",\"agent_uid\":\"%064d\","
                    "\"rtc\":{\"channel\":\"channel\",\"uid\":\"uid\"}}}",
                    0) > 0);
    reset_http_mock(oversized_agent_response);
    assert(mybot_device_client_start_conversation("http://server", "device", "token", "{}",
                                                  &conversation) < 0);

    char oversized_local_response[512];
    assert(snprintf(oversized_local_response, sizeof(oversized_local_response),
                    "{\"data\":{\"conversation_id\":\"c-1\","
                    "\"rtc\":{\"channel\":\"channel\",\"uid\":\"%064d\"}}}",
                    0) > 0);
    reset_http_mock(oversized_local_response);
    assert(mybot_device_client_start_conversation("http://server", "device", "token", "{}",
                                                  &conversation) < 0);

    reset_http_mock("{\"data\":{\"conversation_id\":\"c-1\",\"agent_uid\":12345,"
                    "\"rtc\":{\"app_id\":\"app\",\"channel\":\"channel\","
                    "\"uid\":67890}}}");
    assert(mybot_device_client_start_conversation("http://server", "device", "token", "{}",
                                                  &conversation) == 0);
    assert(strcmp(conversation.rtc_uid, "67890") == 0);
    assert(strcmp(conversation.rtc_agent_uid, "12345") == 0);
}

static void test_renew_failures(void) {
    mybot_device_rtc_token_t token;
    char long_base[700];
    char max_token[MYBOT_DEVICE_CLIENT_MAX_RTC_TOKEN];
    char max_token_response[2048];
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
    assert(strcmp(token.rtc_token, "renewed") == 0);

    memset(max_token, 'r', sizeof(max_token) - 1);
    max_token[sizeof(max_token) - 1] = '\0';
    assert(snprintf(max_token_response, sizeof(max_token_response),
                    "{\"data\":{\"rtc\":{\"channel\":\"channel\","
                    "\"uid\":\"uid\",\"token\":\"%s\"}}}",
                    max_token) > 0);
    reset_http_mock(max_token_response);
    assert(mybot_device_client_renew_rtc_token("http://server", "device", "token", "channel", "uid",
                                               &token) == 0);
    assert(strlen(token.rtc_token) == sizeof(max_token) - 1);
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

#ifdef MYBOT_TEST_WRAP_DEVICE_HEADERS
typedef enum { POST_START_BORROWED, POST_START_GENERATED, POST_RENEW, POST_STOP } device_post_t;

static int call_device_post(device_post_t operation, const char *credential, const char *body) {
    mybot_device_conversation_t conversation;
    mybot_device_rtc_token_t token;
    switch (operation) {
    case POST_START_BORROWED:
    case POST_START_GENERATED:
        return mybot_device_client_start_conversation(
            "http://server", "device", credential, operation == POST_START_BORROWED ? body : NULL,
            &conversation);
    case POST_RENEW:
        return mybot_device_client_renew_rtc_token("http://server", "device", credential,
                                                   "channel-1", "device-uid", &token);
    case POST_STOP:
        return mybot_device_client_stop_conversation("http://server", "device", credential,
                                                     "conversation-1", "device_hangup");
    }
    return -1;
}

static void begin_header_tracking(device_post_t operation, const char *body, bool fail) {
    assert(s_header_allocation == NULL);
    s_header_alloc_calls = 0;
    s_header_free_calls = 0;
    s_fail_header = fail;
    s_borrowed_body = operation == POST_START_BORROWED ? body : NULL;
    s_track_header = true;
    reset_http_mock(operation == POST_STOP    ? NULL
                    : operation == POST_RENEW ? s_valid_renew_response_body
                                              : s_valid_response_body);
}

static void end_header_tracking(int expected_frees) {
    assert(s_header_alloc_calls == 1);
    assert(s_header_free_calls == expected_frees);
    assert(s_header_allocation == NULL);
    s_track_header = false;
    s_fail_header = false;
    s_borrowed_body = NULL;
}

static void test_device_post_header_ownership(void) {
    const struct {
        int transport_result;
        int status;
        int expected;
    } responses[] = {{-1, 200, -1}, {0, 401, 401}, {0, 503, 503}, {0, 0, -1}, {0, 200, 0}};
    char body[] = "{\"custom\":true}";
    for (int operation = POST_START_BORROWED; operation <= POST_STOP; ++operation) {
        begin_header_tracking((device_post_t)operation, body, true);
        int calls_before = s_post_ex_call_count;
        assert(call_device_post((device_post_t)operation, "token", body) == -1);
        assert(s_post_ex_call_count == calls_before);
        assert(strcmp(body, "{\"custom\":true}") == 0);
        end_header_tracking(0);

        begin_header_tracking((device_post_t)operation, body, false);
        calls_before = s_post_ex_call_count;
        assert(call_device_post((device_post_t)operation, "token\nInjected", body) == -1);
        assert(s_post_ex_call_count == calls_before);
        assert(strcmp(body, "{\"custom\":true}") == 0);
        end_header_tracking(1);

        for (size_t i = 0; i < sizeof(responses) / sizeof(responses[0]); ++i) {
            begin_header_tracking((device_post_t)operation, body, false);
            s_http_result = responses[i].transport_result;
            s_response_status = responses[i].status;
            calls_before = s_post_ex_call_count;
            assert(call_device_post((device_post_t)operation, "token", body) ==
                   responses[i].expected);
            assert(s_post_ex_call_count == calls_before + 1);
            assert(strcmp(body, "{\"custom\":true}") == 0);
            end_header_tracking(1);
        }
    }
}
#endif

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
    reset_http_mock("{\"data\":{\"status\":\"pending\",\"device_token\":null}}");
    assert(mybot_device_client_get_binding_status("http://server", "device-1", "Pair token",
                                                  &binding) == 0);
    assert(strcmp(binding.status, "pending") == 0);
    assert(binding.device_token[0] == '\0');
    reset_http_mock("{\"data\":{\"status\":123}}");
    assert(mybot_device_client_get_binding_status("http://server", "device-1", "Pair token",
                                                  &binding) < 0);

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
#ifdef MYBOT_TEST_WRAP_DEVICE_HEADERS
    test_device_post_header_ownership();
#endif
    test_outer_data_contract();
    test_partial_parsed_outputs();
    test_response_parse_allocation_failure();
    aosl_dtor();
    puts("device_client_test: ok");
    return 0;
}
