/* SPDX-License-Identifier: Apache-2.0 */
#include <api/aosl.h>
#include <mybot/platform/mybot_https.h>

#include "mybot_https_internal.h"
#include "platform_test.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/support/mybot_http_client.c" // NOLINT(bugprone-suspicious-include)

static char s_tls_host[128];
static uint16_t s_tls_port;
static char s_tls_request[2048];
static size_t s_tls_request_len;
static size_t s_tls_response_offset;
static int s_tls_closed;
static int s_tls_connect_count;
static int s_tls_send_count;
static int s_tls_recv_count;
static int s_tls_connect_result;
static int s_tls_send_limit;
static int s_tls_recv_limit;
static int s_tls_send_fail_at;
static int s_tls_recv_fail_at;

static const char s_default_tls_response[] =
    "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nsecure";
static const char *s_tls_response = s_default_tls_response;

static void reset_tls_script(const char *response) {
    s_tls_response = response ? response : s_default_tls_response;
    s_tls_connect_result = 0;
    s_tls_send_limit = 0;
    s_tls_recv_limit = 0;
    s_tls_send_fail_at = 0;
    s_tls_recv_fail_at = 0;
}

static int fake_tls_connect(void **connection, const char *host, uint16_t port, int timeout_ms) {
    assert(timeout_ms > 0);
    s_tls_connect_count++;
    assert(snprintf(s_tls_host, sizeof(s_tls_host), "%s", host) < (int)sizeof(s_tls_host));
    s_tls_port = port;
    s_tls_response_offset = 0;
    s_tls_request_len = 0;
    s_tls_send_count = 0;
    s_tls_recv_count = 0;
    if (s_tls_connect_result < 0) {
        return s_tls_connect_result;
    }
    *connection = &s_tls_port;
    return 0;
}

static int fake_tls_send(void *connection, const void *data, size_t len, int timeout_ms) {
    assert(connection == &s_tls_port);
    assert(timeout_ms > 0);
    s_tls_send_count++;
    if (s_tls_send_fail_at == s_tls_send_count) {
        return -1;
    }
    size_t count = len;
    if (s_tls_send_limit > 0 && count > (size_t)s_tls_send_limit) {
        count = (size_t)s_tls_send_limit;
    }
    assert(count < sizeof(s_tls_request) - s_tls_request_len);
    memcpy(s_tls_request + s_tls_request_len, data, count);
    s_tls_request_len += count;
    s_tls_request[s_tls_request_len] = '\0';
    return (int)count;
}

static int fake_tls_recv(void *connection, void *data, size_t capacity, int timeout_ms) {
    assert(connection == &s_tls_port);
    assert(timeout_ms > 0);
    s_tls_recv_count++;
    if (s_tls_recv_fail_at == s_tls_recv_count) {
        return -1;
    }
    size_t response_len = strlen(s_tls_response);
    size_t remaining = response_len - s_tls_response_offset;
    if (remaining == 0) {
        return 0;
    }
    size_t count = remaining < capacity ? remaining : capacity;
    if (s_tls_recv_limit > 0 && count > (size_t)s_tls_recv_limit) {
        count = (size_t)s_tls_recv_limit;
    }
    memcpy(data, s_tls_response + s_tls_response_offset, count);
    s_tls_response_offset += count;
    return (int)count;
}

static void fake_tls_close(void *connection) {
    assert(connection == &s_tls_port);
    s_tls_closed++;
}

static const mybot_https_ops_t s_fake_tls_ops = {
    .name = "fake-tls",
    .connect = fake_tls_connect,
    .send = fake_tls_send,
    .recv = fake_tls_recv,
    .close = fake_tls_close,
};

static void expect_success(const char *raw, int stream_closed, const char *expected_body) {
    mybot_http_client_response_t resp;
    int rc = parse_response(raw, strlen(raw), stream_closed, &resp);
    assert(rc == 0);
    assert(resp.status_code == 200);
    assert(resp.body_len == strlen(expected_body));
    assert(resp.body != NULL);
    assert(strcmp(resp.body, expected_body) == 0);
    mybot_http_client_response_free(&resp);
}

static void expect_failure(const char *raw, int stream_closed) {
    mybot_http_client_response_t resp;
    int rc = parse_response(raw, strlen(raw), stream_closed, &resp);
    assert(rc < 0);
    mybot_http_client_response_free(&resp);
}

static void test_url_and_header_validation(void) {
    url_parts_t parts;
    char long_url[700];

    assert(parse_url(NULL, &parts) < 0);
    assert(parse_url("https://service.example", NULL) < 0);
    assert(parse_url("ftp://service.example/path", &parts) < 0);
    assert(parse_url("https:///path", &parts) < 0);
    assert(parse_url("https://service.example", &parts) == 0);
    assert(strcmp(parts.path, "/") == 0);
    assert(parse_url("https://service.example:65535/a?b=c", &parts) == 0);
    assert(parts.port == 65535);
    assert(parse_url("https://service.example:/path", &parts) < 0);
    assert(parse_url("https://service.example:65536/path", &parts) < 0);
    assert(parse_url("https://service.example:-1/path", &parts) < 0);
    assert(parse_url("https://service.example\\evil/path", &parts) < 0);
    assert(parse_url("https://service.example/path with space", &parts) < 0);
    assert(parse_url("https://service.example/path\\name", &parts) < 0);

    memcpy(long_url, "https://", 8);
    memset(long_url + 8, 'h', 128);
    long_url[136] = '\0';
    assert(parse_url(long_url, &parts) < 0);

    memcpy(long_url, "https://h/", 10);
    memset(long_url + 10, 'p', 512);
    long_url[522] = '\0';
    assert(parse_url(long_url, &parts) < 0);

    assert(!host_is_safe(NULL));
    assert(!host_is_safe(""));
    assert(!host_is_safe("bad@host"));
    assert(!host_is_safe("bad\x7fhost"));
    assert(host_is_safe("good-host.example"));
    assert(!request_target_is_safe(NULL));
    assert(!request_target_is_safe("relative"));
    assert(request_target_is_safe("/safe?query=yes"));
    assert(!header_value_is_safe(NULL));
    assert(!header_value_is_safe(""));
    assert(header_value_is_safe("value\twith-tab"));
    assert(!header_value_is_safe("value\nnewline"));
    assert(header_name_char('A'));
    assert(header_name_char('9'));
    assert(header_name_char('!'));
    assert(!header_name_char(':'));
    assert(extra_headers_are_safe(NULL));
    assert(extra_headers_are_safe(""));
    assert(extra_headers_are_safe("X-Test: one\r\nY_Test:\ttwo\r\n"));
    assert(!extra_headers_are_safe("No-Colon\r\n"));
    assert(!extra_headers_are_safe(": no-name\r\n"));
    assert(!extra_headers_are_safe("Bad Name: value\r\n"));
    assert(!extra_headers_are_safe("X-Test: bad\x7f\r\n"));
    assert(!extra_headers_are_safe("X-Test: value\r\n\r\n"));
}

static void test_response_boundaries(void) {
    mybot_http_client_response_t resp;

    assert(parse_status_line("HTTP/2 404") == 404);
    assert(parse_status_line("HTTP/1.1    204") == 204);
    assert(parse_status_line("NOTHTTP/1.1 200") == 0);
    assert(deadline_remaining_ms(0) == 0);
    assert(deadline_remaining_ms(UINT64_MAX) == INT_MAX);

    int ret = parse_response("HTTP/1.1 204 No Content\r\n\r\n", 27, 1, &resp);
    assert(ret == 0);
    assert(resp.status_code == 204);
    assert(resp.body == NULL);
    assert(resp.body_len == 0);
    mybot_http_client_response_free(&resp);

    {
        const char raw[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 3\r\n\r\nno!trailing";
        ret = parse_response(raw, sizeof(raw) - 1, 1, &resp);
        assert(ret == 0);
        assert(resp.status_code == 404);
        assert(resp.body_len == 3);
        assert(strcmp(resp.body, "no!") == 0);
        mybot_http_client_response_free(&resp);
    }

    expect_success("HTTP/1.1 200 OK\r\n"
                   "Content-Length: 5\r\n"
                   "Content-Length:\t5 \r\n"
                   "\r\n"
                   "helloignored",
                   0, "hello");
    expect_failure("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello", 1);
    expect_failure("HTTP/1.1 200 OK\r\nContent-Length: x\r\n\r\n", 1);
    expect_failure("HTTP/1.1 200 OK\r\nContent-Length: 5x\r\n\r\nhello", 1);
    expect_failure("HTTP/1.1 200 OK\r\nContent-Length: 999999999999999999999999\r\n\r\n", 1);
    expect_failure("HTTP/1.1 200 OK\r\nHeader: value\r\n", 1);
    expect_failure("invalid status\r\n\r\n", 1);
    expect_failure("HTTP/1.1 200 OK", 1);

    expect_success("HTTP/1.1 200 OK\r\n"
                   "Content-Length: 999\r\n"
                   "Transfer-Encoding: chunked\r\n"
                   "\r\n"
                   "5;name=value\r\nhello\r\n6\r\n world\r\n0\r\nX-Trailer: yes\r\n\r\n",
                   0, "hello world");
    expect_failure("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZ\r\nbad\r\n0\r\n\r\n", 1);
    expect_failure("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\naX\r\n0\r\n\r\n", 1);
    expect_failure("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\na\r\n", 1);
    expect_failure("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n", 1);
    expect_failure("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\nextra", 1);
    expect_failure("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFFF\r\n", 1);
}

/* ---- deterministic parser fuzz ---- */
static uint32_t s_http_rng = 0xdeadbeefu;

static uint32_t http_next_rand(void) {
    s_http_rng ^= s_http_rng << 13;
    s_http_rng ^= s_http_rng >> 17;
    s_http_rng ^= s_http_rng << 5;
    return s_http_rng;
}

static void test_parse_fuzz(void) {
    char raw[512];
    for (int iter = 0; iter < 20000; iter++) {
        /* Keep len >= 1 so parse_response never receives an uninitialized
         * buffer (cppcheck uninitvar). */
        size_t len = 1 + (size_t)(http_next_rand() % (sizeof(raw) - 1));
        for (size_t i = 0; i < len; i++) {
            raw[i] = (char)(http_next_rand() & 0xff);
        }
        int closed = (int)(http_next_rand() & 1);
        mybot_http_client_response_t resp;
        int rc = parse_response(raw, len, closed, &resp);
        if (rc == 0) {
            assert(resp.body == NULL || resp.body_len <= len);
            mybot_http_client_response_free(&resp);
        }
    }
}

/* ---- allocator fault injection through linker wrap ---- */
static size_t s_alloc_count;
static size_t s_fail_on_alloc;
static int s_alloc_failures;

void *__wrap_aosl_hal_malloc(size_t size) {
    s_alloc_count++;
    if (s_fail_on_alloc != 0 && s_alloc_count == s_fail_on_alloc) {
        s_alloc_failures++;
        return NULL;
    }
    return malloc(size);
}

void *__wrap_aosl_hal_realloc(void *ptr, size_t size) {
    s_alloc_count++;
    if (s_fail_on_alloc != 0 && s_alloc_count == s_fail_on_alloc) {
        s_alloc_failures++;
        return NULL;
    }
    return realloc(ptr, size);
}

void __wrap_aosl_hal_free(void *ptr) {
    free(ptr);
}

static void test_oom_injection(void) {
    /* The GET against the fake TLS transport performs a small, bounded number
     * of allocations. Fail each one in turn and require a clean -1; past the
     * real allocation count the request must succeed. */
    reset_tls_script(NULL);
    for (size_t fail_at = 1; fail_at <= 32; fail_at++) {
        s_alloc_count = 0;
        s_fail_on_alloc = fail_at;
        s_alloc_failures = 0;

        mybot_http_client_response_t resp;
        memset(&resp, 0, sizeof(resp));
        int rc = mybot_http_client_get("https://api.example.test:8443/status", &resp);

        if (s_alloc_failures == 1) {
            assert(rc == -1);
            assert(resp.body == NULL);
        } else {
            assert(rc == 0);
            mybot_http_client_response_free(&resp);
        }
        s_fail_on_alloc = 0;
    }
}

static void test_tls_transport_and_requests(void) {
    mybot_http_client_response_t resp;
    char long_headers[2100];
    char large_response[6200];

    memset(&resp, 0, sizeof(resp));
    assert(mybot_http_client_get(NULL, &resp) < 0);
    assert(mybot_http_client_get("https://api.example.test", NULL) < 0);
    assert(mybot_http_client_post(NULL, "application/json", "{}", &resp) < 0);
    assert(mybot_http_client_post("https://api.example.test", "application/json", "{}", NULL) < 0);

    reset_tls_script(NULL);
    assert(mybot_http_client_post_ex("https://api.example.test:9443/v1/items", NULL, "payload",
                                     "X-Test: yes\r\n", &resp) == 0);
    assert(strcmp(s_tls_host, "api.example.test") == 0);
    assert(s_tls_port == 9443);
    assert(strstr(s_tls_request, "POST /v1/items HTTP/1.1\r\n") == s_tls_request);
    assert(strstr(s_tls_request, "Content-Type: application/octet-stream\r\n") != NULL);
    assert(strstr(s_tls_request, "Content-Length: 7\r\n") != NULL);
    assert(strstr(s_tls_request, "X-Test: yes\r\n\r\npayload") != NULL);
    mybot_http_client_response_free(&resp);

    reset_tls_script(NULL);
    s_tls_send_limit = 7;
    s_tls_recv_limit = 3;
    assert(mybot_http_client_post("https://api.example.test/data", "application/json", "{}",
                                  &resp) == 0);
    assert(s_tls_send_count > 1);
    assert(s_tls_recv_count > 1);
    assert(strstr(s_tls_request, "POST /data HTTP/1.1\r\n") == s_tls_request);
    assert(strstr(s_tls_request, "Content-Type: application/json\r\n") != NULL);
    assert(strcmp(resp.body, "secure") == 0);
    mybot_http_client_response_free(&resp);

    reset_tls_script(NULL);
    s_tls_connect_result = -1;
    assert(mybot_http_client_get("https://api.example.test/fail", &resp) < 0);

    reset_tls_script(NULL);
    s_tls_send_fail_at = 1;
    assert(mybot_http_client_get("https://api.example.test/fail", &resp) < 0);

    reset_tls_script(NULL);
    s_tls_recv_fail_at = 1;
    assert(mybot_http_client_get("https://api.example.test/fail", &resp) < 0);

    long_headers[0] = 'X';
    long_headers[1] = ':';
    long_headers[2] = ' ';
    memset(long_headers + 3, 'a', 2050);
    memcpy(long_headers + 2053, "\r\n", 3);
    reset_tls_script(NULL);
    assert(mybot_http_client_get_ex("https://api.example.test/too-large", long_headers, &resp) < 0);

    int header_len = snprintf(large_response, sizeof(large_response),
                              "HTTP/1.1 200 OK\r\nContent-Length: 5000\r\n\r\n");
    assert(header_len > 0);
    assert((size_t)header_len + 5000 < sizeof(large_response));
    memset(large_response + header_len, 'z', 5000);
    large_response[header_len + 5000] = '\0';

    reset_tls_script(large_response);
    assert(mybot_http_client_get("https://api.example.test/large", &resp) == 0);
    assert(resp.body_len == 5000);
    assert(resp.body[0] == 'z');
    assert(resp.body[4999] == 'z');
    mybot_http_client_response_free(&resp);

    reset_tls_script(large_response);
    s_alloc_count = 0;
    s_alloc_failures = 0;
    s_fail_on_alloc = 2;
    assert(mybot_http_client_get("https://api.example.test/realloc-fail", &resp) < 0);
    assert(s_alloc_failures == 1);
    s_fail_on_alloc = 0;
    reset_tls_script(NULL);

    mybot_http_client_response_free(NULL);
    memset(&resp, 0, sizeof(resp));
    mybot_http_client_response_free(&resp);
}

int main(void) {
    aosl_ctor();
    mybot_http_client_response_t response;
    memset(&response, 0, sizeof(response));

    assert(!mybot_https_is_registered());
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
    test_url_and_header_validation();

    expect_success("HTTP/1.1 200 OK\r\n"
                   "Content-Length: 5\r\n"
                   "\r\n"
                   "hello",
                   0, "hello");

    expect_success("HTTP/1.1 200 OK\r\n"
                   "cOnTeNt-LeNgTh: 5\r\n"
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

    expect_success("HTTP/1.1 200 OK\r\n"
                   "tRaNsFeR-EnCoDiNg: gzip, ChUnKeD\r\n"
                   "\r\n"
                   "5\r\nhello\r\n0\r\n\r\n",
                   0, "hello");

    expect_failure("HTTP/1.1 200 OK\r\n"
                   "Transfer-Encoding: chunked\r\n"
                   "\r\n"
                   "5\r\nhell",
                   1);

    mybot_platform_descriptor_t descriptor = mybot_test_platform_descriptor();
    descriptor.capabilities |= MYBOT_PLATFORM_CAP_HTTPS;
    descriptor.https = &s_fake_tls_ops;
    assert(mybot_platform_register(&descriptor) == 0);

    int connect_count = s_tls_connect_count;
    assert(mybot_http_client_get("https://good.example/path\r\nX-Injected: yes", &response) < 0);
    assert(mybot_http_client_get("https://good.example@evil.example/path", &response) < 0);
    assert(mybot_http_client_get("https://good.example/path#fragment", &response) < 0);
    assert(mybot_http_client_get_ex("https://good.example/path",
                                    "Authorization: ok\nX-Injected: yes\r\n", &response) < 0);
    assert(mybot_http_client_get_ex("https://good.example/path", "Authorization: ok\r\nmalformed",
                                    &response) < 0);
    assert(mybot_http_client_post_ex("https://good.example/path",
                                     "application/json\r\nX-Injected: yes", "{}", NULL,
                                     &response) < 0);
    assert(s_tls_connect_count == connect_count);

    memset(&response, 0, sizeof(response));
    assert(mybot_http_client_get("https://api.example.test:8443/status", &response) == 0);
    assert(strcmp(s_tls_host, "api.example.test") == 0);
    assert(s_tls_port == 8443);
    assert(strncmp(s_tls_request, "GET /status HTTP/1.1\r\n", 22) == 0);
    assert(strstr(s_tls_request, "Host: api.example.test\r\n") != NULL);
    assert(response.status_code == 200);
    assert(strcmp(response.body, "secure") == 0);
    assert(s_tls_closed == 1);
    assert(s_tls_connect_count == connect_count + 1);
    mybot_http_client_response_free(&response);

    test_response_boundaries();
    test_tls_transport_and_requests();
    test_parse_fuzz();
    test_oom_injection();

    aosl_dtor();
    puts("http_client_test: ok");
    return 0;
}
