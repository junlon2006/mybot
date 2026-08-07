#include <api/aosl.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/support/mybot_http_client.c"

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

    aosl_dtor();
    puts("http_client_test: ok");
    return 0;
}
