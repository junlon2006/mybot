#include "mybot_http_client.h"

#include <hal/aosl_hal_socket.h>
#include <hal/aosl_hal_iomp.h>
#include <hal/aosl_hal_memory.h>
#include <hal/aosl_hal_time.h>
#include <hal/aosl_hal_errno.h>

#include <api/aosl_socket.h>

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <stdint.h>

/* ----------------------------------------------------------
 * Constants
 * ---------------------------------------------------------- */
#define HTTP_DEFAULT_PORT 80
#define HTTP_TIMEOUT_MS 5000 /* deadline for the socket request stages */
#define RECV_BUF_SIZE 4096
#define RECV_BUF_MAX (32 * 1024) /* hard cap on response buffer */
#define MAX_URL_LEN 512

static int deadline_remaining_ms(uint64_t deadline) {
    uint64_t now = aosl_hal_get_tick_ms();
    if (now >= deadline) {
        return 0;
    }

    uint64_t remaining = deadline - now;
    return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

/* ----------------------------------------------------------
 * Internal: URL parts
 * ---------------------------------------------------------- */
typedef struct {
    char host[128];
    int port;
    char path[256];
} url_parts_t;

/*
 * Parse "http://host[:port][/path]" into parts.
 * Returns 0 on success, -1 on error.
 */
static int parse_url(const char *url, url_parts_t *parts) {
    if (!url || !parts) {
        return -1;
    }

    memset(parts, 0, sizeof(*parts));
    parts->port = HTTP_DEFAULT_PORT;
    parts->path[0] = '/';

    const char *p = url;

    /* skip http:// */
    if (strncmp(p, "http://", 7) != 0) {
        return -1;
    }
    p += 7;

    /* extract host (up to ':' or '/' or end) */
    const char *host_start = p;
    while (*p && *p != ':' && *p != '/') {
        p++;
    }
    size_t host_len = (size_t)(p - host_start);
    if (host_len == 0 || host_len >= sizeof(parts->host)) {
        return -1;
    }
    memcpy(parts->host, host_start, host_len);
    parts->host[host_len] = '\0';

    /* optional port */
    if (*p == ':') {
        p++;
        char port_str[8];
        int pi = 0;
        while (*p >= '0' && *p <= '9' && pi < (int)sizeof(port_str) - 1) {
            port_str[pi++] = *p++;
        }
        port_str[pi] = '\0';
        if (pi == 0) {
            return -1;
        }
        parts->port = atoi(port_str);
        if (parts->port <= 0 || parts->port > 65535) {
            return -1;
        }
    }

    /* path (defaults to "/") */
    if (*p == '/') {
        size_t path_len = strlen(p);
        if (path_len >= sizeof(parts->path)) {
            return -1;
        }
        memcpy(parts->path, p, path_len + 1);
    }

    return 0;
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
        return AOSL_INVALID_FD; /* unknown address family */
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
static int send_all(aosl_fd_t fd, const char *data, size_t len, uint64_t deadline) {
    while (len > 0) {
        if (deadline_remaining_ms(deadline) <= 0) {
            return -1;
        }

        int n = aosl_hal_sk_send(fd, data, len, 0);

        if (n == AOSL_HAL_RET_EINTR) {
            /* Interrupted by a signal — the socket state is unchanged, retry
             * the same data. Uses the AOSL errno abstraction so this stays
             * valid across platforms/RTOSes. */
            continue;
        }
        if (n == AOSL_HAL_RET_EAGAIN) {
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
 * Read everything from the socket into a dynamic buffer.
 * Uses a simple loop with recv until connection closes or timeout.
 */
static char *read_all(aosl_fd_t fd, size_t *out_len, int *out_closed, uint64_t deadline) {
    size_t cap = RECV_BUF_SIZE;
    size_t len = 0;
    char *buf = (char *)aosl_hal_malloc(cap);
    *out_closed = 0;
    if (!buf) {
        return NULL;
    }

    while (deadline_remaining_ms(deadline) > 0) {
        int ret = aosl_hal_sk_recv(fd, buf + len, (int)(cap - len - 1), 0);
        if (ret > 0) {
            len += (size_t)ret;
            buf[len] = '\0';

            /* Grow the buffer if needed, bounded by RECV_BUF_MAX so a
             * misbehaving server cannot cause unbounded memory growth. */
            if (cap - len < RECV_BUF_SIZE / 2) {
                if (cap >= RECV_BUF_MAX) {
                    break; /* response exceeds the cap — stop reading */
                }
                cap *= 2;
                if (cap > RECV_BUF_MAX) {
                    cap = RECV_BUF_MAX;
                }
                char *nb = (char *)aosl_hal_realloc(buf, cap);
                if (!nb) {
                    goto fail;
                }
                buf = nb;
            }
        } else if (ret == 0) {
            *out_closed = 1;
            break;
        } else if (ret == AOSL_HAL_RET_EAGAIN) {
            /* No data right now (non-blocking socket). Wait briefly and retry;
             * the deadline bounds the total wait even if the peer is silent. */
            aosl_hal_msleep(10);
        } else {
            /* real error */
            goto fail;
        }
    }

    *out_len = len;
    return buf;

fail:
    aosl_hal_free(buf);
    return NULL;
}

/*
 * Parse HTTP status line: "HTTP/1.x STATUS_TEXT\r\n"
 */
static int parse_status_line(const char *line) {
    /* Expect "HTTP/1.x <CODE> <reason>", e.g. "HTTP/1.1 200 OK". */
    if (strncmp(line, "HTTP/", 5) != 0) {
        return 0;
    }
    line += 5; /* skip "HTTP/" */

    /* Skip the version ("1.0", "1.1", ...). */
    while (*line == '.' || (*line >= '0' && *line <= '9')) {
        line++;
    }

    /* Skip whitespace before the status code. */
    while (*line == ' ') {
        line++;
    }

    int code = 0;
    while (*line >= '0' && *line <= '9') {
        code = code * 10 + (*line++ - '0');
    }
    return code;
}

/*
 * Decode a "Transfer-Encoding: chunked" body into a contiguous buffer.
 * Returns a malloc'd, NUL-terminated buffer (caller frees) and sets *out_len,
 * or NULL if the body is malformed or truncated.
 */
static char *dechunk_body(const char *body, size_t body_len, size_t *out_len) {
    const char *p = body;
    const char *end = body + body_len;
    size_t cap = body_len + 1; /* decoded data never exceeds the raw body */
    char *out = (char *)aosl_hal_malloc(cap);
    size_t len = 0;

    if (!out) {
        return NULL;
    }

    for (;;) {
        size_t chunk_size = 0;
        int has_digit = 0;

        while (p < end) {
            unsigned int digit;
            if (*p >= '0' && *p <= '9') {
                digit = (unsigned int)(*p - '0');
            } else if (*p >= 'a' && *p <= 'f') {
                digit = (unsigned int)(*p - 'a' + 10);
            } else if (*p >= 'A' && *p <= 'F') {
                digit = (unsigned int)(*p - 'A' + 10);
            } else {
                break;
            }
            if (chunk_size > (SIZE_MAX - digit) / 16) {
                goto fail;
            }
            chunk_size = chunk_size * 16 + digit;
            has_digit = 1;
            p++;
        }

        if (!has_digit) {
            goto fail;
        }
        if (p < end && *p == ';') {
            while (p < end && *p != '\r' && *p != '\n') {
                p++;
            }
        }
        if ((size_t)(end - p) < 2 || p[0] != '\r' || p[1] != '\n') {
            goto fail;
        }
        p += 2;

        if (chunk_size == 0) {
            /* Consume optional trailer fields and require their final CRLF. */
            for (;;) {
                const char *line_end = p;
                while ((size_t)(end - line_end) >= 2 &&
                       !(line_end[0] == '\r' && line_end[1] == '\n')) {
                    line_end++;
                }
                if ((size_t)(end - line_end) < 2) {
                    goto fail;
                }
                if (line_end == p) {
                    p = line_end + 2;
                    if (p != end) {
                        goto fail;
                    }
                    out[len] = '\0';
                    *out_len = len;
                    return out;
                }
                p = line_end + 2;
            }
        }

        if (chunk_size > (size_t)(end - p) || chunk_size > cap - len - 1) {
            goto fail;
        }
        memcpy(out + len, p, chunk_size);
        len += chunk_size;
        p += chunk_size;

        if ((size_t)(end - p) < 2 || p[0] != '\r' || p[1] != '\n') {
            goto fail;
        }
        p += 2;
    }

fail:
    aosl_hal_free(out);
    return NULL;
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

/*
 * Parse a complete HTTP response from raw data.
 * Returns the response struct (body will point into or be a copy from raw).
 */
static int parse_response(const char *raw, size_t raw_len, int stream_closed,
                          mybot_http_client_response_t *resp) {
    memset(resp, 0, sizeof(*resp));

    const char *p = raw;
    const char *end = raw + raw_len;

    /* status line */
    const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
    if (!nl) {
        return -1;
    }
    resp->status_code = parse_status_line(p);
    p = nl + 1;

    /* skip CR if present */
    if (p < end && *p == '\r') {
        p++;
    }

    /* headers */
    size_t body_offset = 0;
    size_t content_length = 0;
    int has_content_length = 0;
    int chunked = 0;
    int headers_complete = 0;

    while (p < end) {
        nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        if (!nl) {
            break;
        }

        size_t hdr_len = (size_t)(nl - p);
        /* end of headers: empty line */
        if (hdr_len == 0 || (hdr_len == 1 && *p == '\r')) {
            p = nl + 1;
            if (p < end && *p == '\r') {
                p++;
            }
            body_offset = (size_t)(p - raw);
            headers_complete = 1;
            break;
        }

        /* Parse Content-Length without signed overflow. Repeated fields
         * are accepted only when they carry the same value. */
        if (hdr_len >= 15 && strncasecmp(p, "Content-Length:", 15) == 0) {
            size_t parsed_length;
            if (parse_content_length(p + 15, nl, &parsed_length) < 0 ||
                (has_content_length && content_length != parsed_length)) {
                return -1;
            }
            content_length = parsed_length;
            has_content_length = 1;
        }

        /* parse Transfer-Encoding (takes precedence over Content-Length) */
        if (hdr_len > 18 && strncasecmp(p, "Transfer-Encoding:", 18) == 0) {
            const char *val = p + 18;
            while (val < nl && *val == ' ') {
                val++;
            }
            /* "chunked" may appear in a comma-separated list, e.g. "gzip, chunked" */
            for (const char *v = val; v + 7 <= nl; v++) {
                if (strncasecmp(v, "chunked", 7) == 0) {
                    chunked = 1;
                    break;
                }
            }
        }

        p = nl + 1;
        if (p < end && *p == '\r') {
            p++;
        }
    }

    if (!headers_complete || resp->status_code == 0) {
        return -1;
    }

    size_t avail = raw_len - body_offset;
    if (chunked) {
        resp->body = dechunk_body(raw + body_offset, avail, &resp->body_len);
        return resp->body ? 0 : -1;
    }

    if (has_content_length) {
        if (content_length > avail) {
            return -1;
        }
        resp->body_len = content_length;
    } else {
        if (!stream_closed) {
            return -1;
        }
        resp->body_len = avail;
    }

    if (resp->body_len > 0) {
        resp->body = (char *)aosl_hal_malloc(resp->body_len + 1);
        if (!resp->body) {
            return -1;
        }
        memcpy(resp->body, raw + body_offset, resp->body_len);
        resp->body[resp->body_len] = '\0';
    }

    return 0;
}

/* ----------------------------------------------------------
 * Internal: common request logic
 * ---------------------------------------------------------- */
static int http_request(const char *method, const char *url, const char *content_type,
                        const char *req_body, const char *extra_headers,
                        mybot_http_client_response_t *resp) {
    uint64_t deadline = aosl_hal_get_tick_ms() + HTTP_TIMEOUT_MS;

    url_parts_t parts;
    if (parse_url(url, &parts) < 0) {
        return -1;
    }

    aosl_fd_t fd = tcp_connect(parts.host, parts.port, deadline);
    if (aosl_fd_invalid(fd)) {
        return -1;
    }

    /* Build HTTP request */
    char req[2048];
    int req_len;

    if (strcmp(method, "POST") == 0 && req_body) {
        req_len = snprintf(req, sizeof(req),
                           "POST %s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %zu\r\n"
                           "Connection: close\r\n"
                           "%s" /* extra headers inserted here */
                           "\r\n"
                           "%s",
                           parts.path, parts.host,
                           content_type ? content_type : "application/octet-stream",
                           strlen(req_body), extra_headers ? extra_headers : "", req_body);
    } else {
        req_len = snprintf(req, sizeof(req),
                           "GET %s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "Connection: close\r\n"
                           "%s" /* extra headers inserted here */
                           "\r\n",
                           parts.path, parts.host, extra_headers ? extra_headers : "");
    }

    if (req_len < 0 || (size_t)req_len >= sizeof(req)) {
        aosl_hal_sk_close(fd);
        return -1;
    }

    /* Send request */
    int ret = send_all(fd, req, (size_t)req_len, deadline);
    if (ret < 0) {
        aosl_hal_sk_close(fd);
        return -1;
    }

    /* Read response */
    size_t raw_len = 0;
    int stream_closed = 0;
    char *raw = read_all(fd, &raw_len, &stream_closed, deadline);
    aosl_hal_sk_close(fd);

    if (!raw) {
        return -1;
    }

    /* Parse */
    ret = parse_response(raw, raw_len, stream_closed, resp);
    aosl_hal_free(raw);

    return ret;
}

/* ----------------------------------------------------------
 * Public API
 * ---------------------------------------------------------- */

int mybot_http_client_get(const char *url, mybot_http_client_response_t *resp) {
    return mybot_http_client_get_ex(url, NULL, resp);
}

int mybot_http_client_post(const char *url, const char *content_type, const char *body,
                           mybot_http_client_response_t *resp) {
    return mybot_http_client_post_ex(url, content_type, body, NULL, resp);
}

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
