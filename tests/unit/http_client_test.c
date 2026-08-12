/* SPDX-License-Identifier: Apache-2.0 */
#include <api/aosl.h>
#include <mybot/platform/mybot_https.h>

#include "mybot_https_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/support/mybot_http_client.c" // NOLINT(bugprone-suspicious-include)

static char s_tls_host[128];
static uint16_t s_tls_port;
static char s_tls_request[2048];
static size_t s_tls_response_offset;
static int s_tls_closed;
static int s_tls_connect_count;

static const char s_tls_response[] =
    "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nsecure";

static int fake_tls_connect(void **connection, const char *host, uint16_t port, int timeout_ms) {
    assert(timeout_ms > 0);
    s_tls_connect_count++;
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

    mybot_https_ops_t incomplete_ops = s_fake_tls_ops;
    incomplete_ops.recv = NULL;
    assert(mybot_https_register(NULL) < 0);
    assert(mybot_https_register(&incomplete_ops) < 0);
    assert(mybot_https_register(&s_fake_tls_ops) == 0);
    assert(mybot_https_register(&s_fake_tls_ops) < 0);

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

    test_parse_fuzz();
    test_oom_injection();

    aosl_dtor();
    puts("http_client_test: ok");
    return 0;
}
