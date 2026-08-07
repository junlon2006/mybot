#include <api/aosl.h>
#include <mybot/platform/mybot_https_transport.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/support/mybot_http_client.c"

static char s_tls_host[128];
static uint16_t s_tls_port;
static char s_tls_request[2048];
static size_t s_tls_response_offset;
static int s_tls_closed;

static const char s_tls_response[] =
    "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nsecure";

static int fake_tls_connect(void **connection, const char *host, uint16_t port, int timeout_ms) {
    assert(timeout_ms > 0);
    assert(snprintf(s_tls_host, sizeof(s_tls_host), "%s", host) < (int)sizeof(s_tls_host));
    s_tls_port = port;
    s_tls_response_offset = 0;
    *connection = &s_tls_port;
    return 0;
}

static int fake_tls_send(void *connection, const void *data, size_t len, int timeout_ms) {
    assert(connection == &s_tls_port);
    assert(timeout_ms > 0);
    assert(len < sizeof(s_tls_request));
    memcpy(s_tls_request, data, len);
    s_tls_request[len] = '\0';
    return (int)len;
}

static int fake_tls_recv(void *connection, void *data, size_t capacity, int timeout_ms) {
    assert(connection == &s_tls_port);
    assert(timeout_ms > 0);
    size_t remaining = sizeof(s_tls_response) - 1 - s_tls_response_offset;
    if (remaining == 0) {
        return 0;
    }
    size_t count = remaining < capacity ? remaining : capacity;
    memcpy(data, s_tls_response + s_tls_response_offset, count);
    s_tls_response_offset += count;
    return (int)count;
}

static void fake_tls_close(void *connection) {
    assert(connection == &s_tls_port);
    s_tls_closed++;
}

static const mybot_https_transport_ops_t s_fake_tls_ops = {
    .name = "fake-tls",
    .connect = fake_tls_connect,
    .send = fake_tls_send,
    .recv = fake_tls_recv,
    .close = fake_tls_close,
};

static void expect_success(const char *raw, int stream_closed, const char *expected_body) {
    mybot_http_client_response_t resp;
    assert(parse_response(raw, strlen(raw), stream_closed, &resp) == 0);
    assert(resp.status_code == 200);
    assert(resp.body_len == strlen(expected_body));
    assert(resp.body != NULL);
    assert(strcmp(resp.body, expected_body) == 0);
    mybot_http_client_response_free(&resp);
}

static void expect_failure(const char *raw, int stream_closed) {
    mybot_http_client_response_t resp;
    assert(parse_response(raw, strlen(raw), stream_closed, &resp) < 0);
    mybot_http_client_response_free(&resp);
}

int main(void) {
    aosl_ctor();
    mybot_http_client_response_t response;
    memset(&response, 0, sizeof(response));

    assert(!mybot_https_transport_is_registered());
    assert(mybot_http_client_get("https://api.example.test/status", &response) < 0);
    assert(response.body == NULL);

    url_parts_t parts;
    assert(parse_url("https://service.example/v1", &parts) == 0);
    assert(parts.use_tls);
    assert(parts.port == 443);
    assert(strcmp(parts.host, "service.example") == 0);
    assert(strcmp(parts.path, "/v1") == 0);
    assert(parse_url("http://service.example/v1", &parts) < 0);
    assert(parse_url("https://service.example:0/v1", &parts) < 0);
    assert(parse_url("https://service.example:443x/v1", &parts) < 0);

    expect_success("HTTP/1.1 200 OK\r\n"
                   "Content-Length: 5\r\n"
                   "\r\n"
                   "hello",
                   0, "hello");

    expect_failure("HTTP/1.1 200 OK\r\n"
                   "Content-Length: 6\r\n"
                   "\r\n"
                   "hello",
                   1);

    expect_failure("HTTP/1.1 200 OK\r\n"
                   "\r\n"
                   "hello",
                   0);
    expect_success("HTTP/1.1 200 OK\r\n"
                   "\r\n"
                   "hello",
                   1, "hello");

    expect_failure("HTTP/1.1 200 OK\r\n"
                   "Content-Length: 0\r\n",
                   1);

    expect_success("HTTP/1.1 200 OK\r\n"
                   "Transfer-Encoding: chunked\r\n"
                   "\r\n"
                   "5\r\nhello\r\n0\r\n\r\n",
                   0, "hello");

    expect_failure("HTTP/1.1 200 OK\r\n"
                   "Transfer-Encoding: chunked\r\n"
                   "\r\n"
                   "5\r\nhell",
                   1);

    mybot_https_transport_ops_t incomplete_ops = s_fake_tls_ops;
    incomplete_ops.recv = NULL;
    assert(mybot_https_transport_register(NULL) < 0);
    assert(mybot_https_transport_register(&incomplete_ops) < 0);
    assert(mybot_https_transport_register(&s_fake_tls_ops) == 0);
    assert(mybot_https_transport_register(&s_fake_tls_ops) < 0);
    memset(&response, 0, sizeof(response));
    assert(mybot_http_client_get("https://api.example.test:8443/status", &response) == 0);
    assert(strcmp(s_tls_host, "api.example.test") == 0);
    assert(s_tls_port == 8443);
    assert(strncmp(s_tls_request, "GET /status HTTP/1.1\r\n", 22) == 0);
    assert(strstr(s_tls_request, "Host: api.example.test\r\n") != NULL);
    assert(response.status_code == 200);
    assert(strcmp(response.body, "secure") == 0);
    assert(s_tls_closed == 1);
    mybot_http_client_response_free(&response);

    aosl_dtor();
    puts("http_client_test: ok");
    return 0;
}
