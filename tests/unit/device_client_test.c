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
static const char *s_response_body;

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
    (void)resp;
    return -1;
}

int mybot_http_client_get_ex(const char *url, const char *extra_headers,
                             mybot_http_client_response_t *resp) {
    (void)url;
    (void)extra_headers;
    (void)resp;
    return -1;
}

int mybot_http_client_post_ex(const char *url, const char *content_type, const char *body,
                              const char *extra_headers, mybot_http_client_response_t *resp) {
    (void)url;
    (void)content_type;
    (void)extra_headers;

    int written = snprintf(s_request_body, sizeof(s_request_body), "%s", body);
    if (written < 0 || (size_t)written >= sizeof(s_request_body)) {
        return -1;
    }

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

void mybot_http_client_response_free(mybot_http_client_response_t *resp) {
    if (!resp) {
        return;
    }
    free(resp->body);
    memset(resp, 0, sizeof(*resp));
}

int main(void) {
    aosl_ctor();

    mybot_device_conversation_t conversation;
    char long_id[MYBOT_DEVICE_CLIENT_MAX_ID + 1];
    char long_id_response_body[1024];
    s_response_body = s_valid_response_body;
    assert(mybot_device_client_start_conversation("http://server", "device-1", "token", NULL,
                                                  &conversation) == 0);
    assert(strcmp(conversation.conversation_id, "conversation-1") == 0);

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
