/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_http_client.h"
#include "mybot_https_transport.h"
#include "mybot_url.h"

#include <mybot/mybot_build_config.h>

#include <hal/aosl_hal_socket.h>
#include <hal/aosl_hal_iomp.h>
#include <hal/aosl_hal_memory.h>
#include <hal/aosl_hal_time.h>
#include <hal/aosl_hal_errno.h>

#include <api/aosl_socket.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <stdint.h>

/* ----------------------------------------------------------
 * Constants
 * ---------------------------------------------------------- */
#define HTTP_TIMEOUT_MS 5000 /* shared deadline for connect/send/receive stages */
#define RECV_BUF_SIZE 4096
#define RECV_BUF_MAX (32 * 1024) /* hard cap on response buffer */
#define HTTP_REQUEST_BUFFER_SIZE 2048

static bool ascii_case_equal_n(const char *left, const char *right, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char left_char = (unsigned char)left[i];
        unsigned char right_char = (unsigned char)right[i];
        if (left_char >= 'A' && left_char <= 'Z') {
            left_char = (unsigned char)(left_char + ('a' - 'A'));
        }
        if (right_char >= 'A' && right_char <= 'Z') {
            right_char = (unsigned char)(right_char + ('a' - 'A'));
        }
        if (left_char != right_char) {
            return false;
        }
    }
    return true;
}

static int deadline_remaining_ms(uint64_t deadline) {
    uint64_t now = aosl_hal_get_tick_ms();
    if (now >= deadline) {
        return 0;
    }

    uint64_t remaining = deadline - now;
    return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

static bool header_name_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '!' ||
           c == '#' || c == '$' || c == '%' || c == '&' || c == '*' || c == '+' || c == '-' ||
           c == '.' || c == '^' || c == '_' || c == '|' || c == '~';
}

static bool extra_headers_are_safe(const char *headers) {
    if (!headers || !headers[0]) {
        return true;
    }

    const char *line = headers;
    while (*line) {
        const char *line_end = strstr(line, "\r\n");
        if (!line_end || line_end == line) {
            return false;
        }
        const char *colon = memchr(line, ':', (size_t)(line_end - line));
        if (!colon || colon == line) {
            return false;
        }
        for (const unsigned char *p = (const unsigned char *)line; p < (const unsigned char *)colon;
             p++) {
            if (!header_name_char(*p)) {
                return false;
            }
        }
        for (const unsigned char *p = (const unsigned char *)colon + 1;
             p < (const unsigned char *)line_end; p++) {
            if ((*p < 0x20 && *p != '\t') || *p > 0x7e) {
                return false;
            }
        }
        line = line_end + 2;
    }
    return true;
}

static aosl_fd_t tcp_connect(const char *host, int port, uint64_t deadline);
typedef struct {
    bool use_tls;
    aosl_fd_t fd;
    void *tls_connection;
} http_stream_t;

static int stream_connect(http_stream_t *stream, const mybot_url_parts_t *parts,
                          uint64_t deadline) {
    memset(stream, 0, sizeof(*stream));
    stream->fd = AOSL_INVALID_FD;
    stream->use_tls = parts->use_tls;
    if (parts->use_tls) {
#if MYBOT_ENABLE_HTTPS
        return mybot_https_transport_connect(&stream->tls_connection, parts->host,
                                             (uint16_t)parts->port,
                                             deadline_remaining_ms(deadline));
#else
        return -1;
#endif
    }
#if MYBOT_ALLOW_INSECURE_HTTP
    stream->fd = tcp_connect(parts->host, parts->port, deadline);
    return aosl_fd_invalid(stream->fd) ? -1 : 0;
#else
    return -1;
#endif
}

static int stream_send(http_stream_t *stream, const void *data, size_t len, uint64_t deadline) {
    if (stream->use_tls) {
        return mybot_https_transport_send(stream->tls_connection, data, len,
                                          deadline_remaining_ms(deadline));
    }
    return aosl_hal_sk_send(stream->fd, data, len, 0);
}

static int stream_recv(http_stream_t *stream, void *data, size_t capacity, uint64_t deadline) {
    if (stream->use_tls) {
        return mybot_https_transport_recv(stream->tls_connection, data, capacity,
                                          deadline_remaining_ms(deadline));
    }
    return aosl_hal_sk_recv(stream->fd, data, capacity, 0);
}

static void stream_close(http_stream_t *stream) {
    if (stream->use_tls) {
        mybot_https_transport_close(stream->tls_connection);
        stream->tls_connection = NULL;
    } else if (!aosl_fd_invalid(stream->fd)) {
        aosl_hal_sk_close(stream->fd);
        stream->fd = AOSL_INVALID_FD;
    }
}

/* ----------------------------------------------------------
 * Internal: socket helpers via AOSL HAL
 * ---------------------------------------------------------- */

/*
 * Create a TCP socket and connect to host:port.
 * Returns socket fd, or AOSL_INVALID_FD on error.
 */
static int wait_for_connect(aosl_fd_t fd, uint64_t deadline) {
    fd_set_t write_fds = aosl_hal_fdset_create();
    fd_set_t error_fds = aosl_hal_fdset_create();
    if (!write_fds || !error_fds) {
        if (write_fds) {
            aosl_hal_fdset_destroy(write_fds);
        }
        if (error_fds) {
            aosl_hal_fdset_destroy(error_fds);
        }
        return -1;
    }

    int result = -1;
    for (;;) {
        int timeout_ms = deadline_remaining_ms(deadline);
        if (timeout_ms <= 0) {
            break;
        }

        aosl_hal_fdset_zero(write_fds);
        aosl_hal_fdset_zero(error_fds);
        aosl_hal_fdset_set(write_fds, fd);
        aosl_hal_fdset_set(error_fds, fd);

        int ret = aosl_hal_select((int)fd + 1, NULL, write_fds, error_fds, timeout_ms);
        if (ret == AOSL_HAL_RET_EINTR) {
            continue;
        }
        if (ret <= 0 || aosl_hal_fdset_isset(error_fds, fd)) {
            break;
        }
        if (aosl_hal_fdset_isset(write_fds, fd)) {
            /* A writable socket may still carry a deferred connect error.
             * AOSL does not expose portable SO_ERROR querying, so let the
             * first real send report that error. */
            result = 0;
            break;
        }
    }

    aosl_hal_fdset_destroy(write_fds);
    aosl_hal_fdset_destroy(error_fds);
    return result;
}

static aosl_fd_t tcp_connect(const char *host, int port, uint64_t deadline) {
    aosl_sockaddr_t addrs[8];
    int count = aosl_hal_gethostbyname(host, addrs, (int)(sizeof(addrs) / sizeof(addrs[0])));
    if (count < 1 || deadline_remaining_ms(deadline) <= 0) {
        return AOSL_INVALID_FD;
    }

    /* Prefer an IPv4 address; fall back to IPv6 if that is all the resolver
     * returned. getaddrinfo() is AF_UNSPEC and may list IPv6 first (e.g.
     * "::1" for localhost), which would not match the socket family below. */
    const aosl_sockaddr_t *addr = &addrs[0];
    enum aosl_socket_domain domain = AOSL_AF_INET;
    for (int i = 0; i < count; i++) {
        if (addrs[i].sa_family == AOSL_AF_INET) {
            addr = &addrs[i];
            break;
        }
    }
    if (addr->sa_family == AOSL_AF_INET6) {
        domain = AOSL_AF_INET6;
    } else if (addr->sa_family != AOSL_AF_INET) {
        return AOSL_INVALID_FD; /* Unknown address family. */
    }

    /* Fix the port (the resolver does not set it). */
    aosl_sockaddr_t target = *addr;
    target.sa_port = aosl_htons((uint16_t)port);

    aosl_fd_t fd = aosl_hal_sk_socket(domain, AOSL_SOCK_STREAM, AOSL_IPPROTO_TCP);
    if (aosl_fd_invalid(fd)) {
        return AOSL_INVALID_FD;
    }

    /* Non-blocking must be enabled before connect so the request deadline also
     * bounds the TCP handshake. */
    if (aosl_hal_sk_set_nonblock(fd) < 0) {
        aosl_hal_sk_close(fd);
        return AOSL_INVALID_FD;
    }

    int ret = aosl_hal_sk_connect(fd, &target);
    if (ret < 0 && ret != AOSL_HAL_RET_EINPROGRESS && ret != AOSL_HAL_RET_EAGAIN) {
        aosl_hal_sk_close(fd);
        return AOSL_INVALID_FD;
    }
    if (ret < 0 && wait_for_connect(fd, deadline) < 0) {
        aosl_hal_sk_close(fd);
        return AOSL_INVALID_FD;
    }

    return fd;
}

/*
 * Send all bytes (retry on short send).
 * Returns 0 on success, -1 on error.
 */
static int send_all(http_stream_t *stream, const char *data, size_t len, uint64_t deadline) {
    while (len > 0) {
        if (deadline_remaining_ms(deadline) <= 0) {
            return -1;
        }

        int n = stream_send(stream, data, len, deadline);

        if (!stream->use_tls && n == AOSL_HAL_RET_EINTR) {
            /* Interrupted by a signal — the socket state is unchanged, retry
             * the same data. Uses the AOSL errno abstraction so this stays
             * valid across platforms/RTOSes. */
            continue;
        }
        if (!stream->use_tls && n == AOSL_HAL_RET_EAGAIN) {
            /* Would block. The shared request deadline bounds retries. */
            aosl_hal_msleep(1);
            continue;
        }
        if (n < 0) {
            return -1;
        }

        data += n;
        len -= (size_t)n;
    }
    return 0;
}

/* ----------------------------------------------------------
 * Internal: HTTP response reading
 * ---------------------------------------------------------- */

/*
 * Parse an HTTP status line such as "HTTP/1.1 200 OK\r\n".
 */
static int parse_status_line(const char *line) {
    /* RFC 9112 status-line: HTTP-version SP 3DIGIT SP reason-phrase. */
    if (!line || strncmp(line, "HTTP/1.", 7) != 0 || (line[7] != '0' && line[7] != '1') ||
        line[8] != ' ') {
        return -1;
    }
    if (line[9] < '0' || line[9] > '9' || line[10] < '0' || line[10] > '9' || line[11] < '0' ||
        line[11] > '9' || line[12] != ' ') {
        return -1;
    }
    return (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
}

static int parse_content_length(const char *value, const char *end, size_t *out_length) {
    size_t length = 0;
    int has_digit = 0;

    while (value < end && (*value == ' ' || *value == '\t')) {
        value++;
    }
    while (value < end && *value >= '0' && *value <= '9') {
        unsigned int digit = (unsigned int)(*value - '0');
        if (length > (SIZE_MAX - digit) / 10) {
            return -1;
        }
        length = length * 10 + digit;
        has_digit = 1;
        value++;
    }
    while (value < end && (*value == ' ' || *value == '\t')) {
        value++;
    }
    if (value < end && *value == '\r') {
        value++;
    }
    if (!has_digit || value != end) {
        return -1;
    }

    *out_length = length;
    return 0;
}

typedef enum {
    HTTP_STATUS_LINE,
    HTTP_HEADERS,
    HTTP_FIXED_BODY,
    HTTP_EOF_BODY,
    HTTP_CHUNK_SIZE,
    HTTP_CHUNK_DATA,
    HTTP_CHUNK_TRAILERS,
} http_response_phase_t;

/* Offsets survive receive-buffer growth. Completed chunks are compacted into the body area. */
typedef struct {
    http_response_phase_t phase;
    size_t offset;
    size_t scan;
    size_t body_start;
    size_t body_len;
    size_t chunk_size;
    bool has_length;
    bool chunked;
    int status_code;
} http_response_parser_t;

static bool has_chunked_encoding(const char *val, const char *nl) {
    while (val < nl) {
        while (val < nl && (*val == ' ' || *val == '\t' || *val == ',')) {
            val++;
        }
        const char *token = val;
        while (val < nl && *val != ',' && *val != ';' && *val != ' ' && *val != '\t' &&
               *val != '\r' && *val != '\n') {
            val++;
        }
        size_t token_len = (size_t)(val - token);
        if (token_len == 7 && ascii_case_equal_n(token, "chunked", 7)) {
            return true;
        }
        while (val < nl && *val != ',') {
            val++;
        }
    }
    return false;
}

static int parse_header(http_response_parser_t *parser, const char *line, const char *nl) {
    size_t len = (size_t)(nl - line);
    if (len >= 15 && ascii_case_equal_n(line, "Content-Length:", 15)) {
        size_t length;
        if (parse_content_length(line + 15, nl, &length) < 0 ||
            (parser->has_length && parser->body_len != length)) {
            return -1;
        }
        parser->has_length = true;
        parser->body_len = length;
    } else if (len > 18 && ascii_case_equal_n(line, "Transfer-Encoding:", 18) &&
               has_chunked_encoding(line + 18, nl)) {
        parser->chunked = true;
    }
    return 0;
}

static int parse_chunk_size(const char *line, const char *end, size_t *size) {
    size_t length = 0;
    const char *p = line;
    while (p < end && *p != ';') {
        unsigned digit;
        if (*p >= '0' && *p <= '9') {
            digit = (unsigned)(*p - '0');
        } else if (*p >= 'a' && *p <= 'f') {
            digit = (unsigned)(*p - 'a') + 10U;
        } else if (*p >= 'A' && *p <= 'F') {
            digit = (unsigned)(*p - 'A') + 10U;
        } else {
            return -1;
        }
        if (length > (SIZE_MAX - digit) / 16U) {
            return -1;
        }
        length = length * 16U + digit;
        ++p;
    }
    if (p == line || memchr(p, '\r', (size_t)(end - p))) {
        return -1;
    }
    *size = length;
    return 0;
}

static int parse_response_line(http_response_parser_t *parser, const char *line, const char *nl,
                               size_t len) {
    if (parser->phase == HTTP_STATUS_LINE) {
        if (nl - line < 13) {
            return -1;
        }
        parser->status_code = parse_status_line(line);
        parser->phase = HTTP_HEADERS;
        return parser->status_code > 0 ? 0 : -1;
    }
    if (parser->phase == HTTP_HEADERS) {
        if (nl != line && (nl - line != 1 || *line != '\r')) {
            return parse_header(parser, line, nl);
        }
        parser->body_start = parser->offset;
        if (parser->chunked) {
            parser->body_len = 0;
            parser->phase = HTTP_CHUNK_SIZE;
        } else {
            parser->phase = parser->has_length ? HTTP_FIXED_BODY : HTTP_EOF_BODY;
        }
        return 0;
    }

    if (nl == line || nl[-1] != '\r') {
        return -1;
    }
    if (parser->phase == HTTP_CHUNK_TRAILERS) {
        if (nl - line == 1) {
            return parser->offset == len ? 1 : -1;
        }
        return 0;
    }
    if (parse_chunk_size(line, nl - 1, &parser->chunk_size) < 0) {
        return -1;
    }
    parser->phase = parser->chunk_size ? HTTP_CHUNK_DATA : HTTP_CHUNK_TRAILERS;
    return 0;
}

/* Return 1 when the response is complete, 0 when another fragment is needed, or -1 on error. */
static int advance_response(http_response_parser_t *parser, char *raw, size_t len, bool closed) {
    for (;;) {
        size_t available = len - parser->offset;
        if (parser->phase == HTTP_FIXED_BODY) {
            return available >= parser->body_len;
        }
        if (parser->phase == HTTP_EOF_BODY) {
            parser->body_len = available;
            return closed;
        }
        if (parser->phase == HTTP_CHUNK_DATA) {
            size_t size = parser->chunk_size;
            if (available < size || available - size < 2) {
                return 0;
            }
            if (memcmp(raw + parser->offset + size, "\r\n", 2) != 0) {
                return -1;
            }
            memmove(raw + parser->body_start + parser->body_len, raw + parser->offset, size);
            parser->body_len += size;
            parser->offset += size + 2;
            parser->scan = parser->offset;
            parser->phase = HTTP_CHUNK_SIZE;
            continue;
        }

        const char *nl = memchr(raw + parser->scan, '\n', len - parser->scan);
        if (!nl) {
            parser->scan = len;
            return 0;
        }
        const char *line = raw + parser->offset;
        parser->offset = (size_t)(nl - raw) + 1;
        parser->scan = parser->offset;

        int result = parse_response_line(parser, line, nl, len);
        if (result != 0) {
            return result;
        }
    }
}

static int copy_response(const http_response_parser_t *parser, const char *raw,
                         mybot_http_client_response_t *resp) {
    if (parser->body_len > 0 || parser->chunked) {
        resp->body = (char *)aosl_hal_malloc(parser->body_len + 1);
        if (!resp->body) {
            return -1;
        }
        memcpy(resp->body, raw + parser->body_start, parser->body_len);
        resp->body[parser->body_len] = '\0';
    }
    resp->status_code = parser->status_code;
    resp->body_len = parser->body_len;
    return 0;
}

static int read_response(http_stream_t *stream, uint64_t deadline,
                         mybot_http_client_response_t *resp) {
    http_response_parser_t parser = {0};
    size_t cap = RECV_BUF_SIZE;
    size_t len = 0;
    char *buf = (char *)aosl_hal_malloc(cap);
    if (!buf) {
        return -1;
    }

    memset(resp, 0, sizeof(*resp));
    int result = -1;
    while (deadline_remaining_ms(deadline) > 0) {
        size_t capacity = cap - len - 1;
        /* At the cap, distinguish a clean EOF from an oversized response
         * without issuing a zero-length receive or overwriting the NUL. */
        char overflow_byte;
        int ret = stream_recv(stream, capacity ? buf + len : &overflow_byte,
                              capacity ? capacity : 1, deadline);
        if (!stream->use_tls && ret == AOSL_HAL_RET_EAGAIN) {
            aosl_hal_msleep(10);
            continue;
        }
        if (ret < 0 || (size_t)ret > capacity) {
            break;
        }
        len += (size_t)ret;
        buf[len] = '\0';

        int progress = advance_response(&parser, buf, len, ret == 0);
        if (progress > 0) {
            result = copy_response(&parser, buf, resp);
            break;
        }
        if (ret == 0 || progress < 0) {
            break;
        }

        if (cap < RECV_BUF_MAX && cap - len < RECV_BUF_SIZE / 2) {
            cap *= 2;
            if (cap > RECV_BUF_MAX) {
                cap = RECV_BUF_MAX;
            }
            char *larger = (char *)aosl_hal_realloc(buf, cap);
            if (!larger) {
                break;
            }
            buf = larger;
        }
    }

    aosl_hal_free(buf);
    return result;
}

/* ----------------------------------------------------------
 * Internal: common request logic
 * ---------------------------------------------------------- */
static int http_request(const char *method, const char *url, const char *content_type,
                        const char *req_body, const char *extra_headers,
                        mybot_http_client_response_t *resp) {
    uint64_t deadline = aosl_hal_get_tick_ms() + HTTP_TIMEOUT_MS;

    mybot_url_parts_t parts;
    if (mybot_url_parse(url, &parts) < 0) {
        return -1;
    }
    if ((content_type && !mybot_http_header_value_is_safe(content_type, true)) ||
        !extra_headers_are_safe(extra_headers)) {
        return -1;
    }

    http_stream_t stream;
    if (stream_connect(&stream, &parts, deadline) < 0) {
        return -1;
    }

    /* Build the HTTP request. Include a non-default port in Host so virtual
     * hosts and reverse proxies route the request correctly. */
    char *req = (char *)aosl_hal_malloc(HTTP_REQUEST_BUFFER_SIZE);
    if (!req) {
        stream_close(&stream);
        return -1;
    }
    char host_header[sizeof(parts.host) + 8];
    bool include_port = (parts.use_tls && parts.port != MYBOT_URL_HTTPS_PORT) ||
                        (!parts.use_tls && parts.port != MYBOT_URL_HTTP_PORT);
    int host_len = include_port
                       ? snprintf(host_header, sizeof(host_header), "%s:%d", parts.host, parts.port)
                       : snprintf(host_header, sizeof(host_header), "%s", parts.host);
    if (host_len < 0 || (size_t)host_len >= sizeof(host_header)) {
        aosl_hal_free(req);
        stream_close(&stream);
        return -1;
    }
    int req_len;

    if (strcmp(method, "POST") == 0 && req_body) {
        req_len = snprintf(req, HTTP_REQUEST_BUFFER_SIZE,
                           "POST %s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %zu\r\n"
                           "Connection: close\r\n"
                           "%s" /* extra headers inserted here */
                           "\r\n"
                           "%s",
                           parts.path, host_header,
                           content_type ? content_type : "application/octet-stream",
                           strlen(req_body), extra_headers ? extra_headers : "", req_body);
    } else {
        req_len = snprintf(req, HTTP_REQUEST_BUFFER_SIZE,
                           "GET %s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "Connection: close\r\n"
                           "%s" /* extra headers inserted here */
                           "\r\n",
                           parts.path, host_header, extra_headers ? extra_headers : "");
    }

    if (req_len < 0 || (size_t)req_len >= HTTP_REQUEST_BUFFER_SIZE) {
        aosl_hal_free(req);
        stream_close(&stream);
        return -1;
    }

    /* Send the request. */
    int ret = send_all(&stream, req, (size_t)req_len, deadline);
    aosl_hal_free(req);
    if (ret < 0) {
        stream_close(&stream);
        return -1;
    }

    ret = read_response(&stream, deadline, resp);
    stream_close(&stream);
    return ret;
}

/* ----------------------------------------------------------
 * Public API
 * ---------------------------------------------------------- */

int mybot_http_client_get_ex(const char *url, const char *extra_headers,
                             mybot_http_client_response_t *resp) {
    if (!url || !resp) {
        return -1;
    }
    return http_request("GET", url, NULL, NULL, extra_headers, resp);
}

int mybot_http_client_post_ex(const char *url, const char *content_type, const char *body,
                              const char *extra_headers, mybot_http_client_response_t *resp) {
    if (!url || !resp) {
        return -1;
    }
    return http_request("POST", url, content_type, body, extra_headers, resp);
}

void mybot_http_client_response_free(mybot_http_client_response_t *resp) {
    if (resp) {
        if (resp->body) {
            aosl_hal_free(resp->body);
            resp->body = NULL;
        }
        resp->body_len = 0;
        resp->status_code = 0;
    }
}
