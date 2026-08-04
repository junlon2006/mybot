#include "http_client.h"

#include <hal/aosl_hal_socket.h>
#include <hal/aosl_hal_memory.h>
#include <hal/aosl_hal_time.h>

#include <api/aosl_socket.h>

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

/* ----------------------------------------------------------
 * Constants
 * ---------------------------------------------------------- */
#define HTTP_DEFAULT_PORT   80
#define HTTP_TIMEOUT_MS     5000   /* connect/recv timeout */
#define RECV_BUF_SIZE       4096
#define MAX_URL_LEN         512

/* ----------------------------------------------------------
 * Internal: URL parts
 * ---------------------------------------------------------- */
typedef struct {
    char host[128];
    int  port;
    char path[256];
} url_parts_t;

/*
 * Parse "http://host[:port][/path]" into parts.
 * Returns 0 on success, -1 on error.
 */
static int parse_url(const char *url, url_parts_t *parts)
{
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
static aosl_fd_t tcp_connect(const char *host, int port)
{
    aosl_sockaddr_t addr;
    int count = aosl_hal_gethostbyname(host, &addr, 1);
    if (count < 1) {
        return AOSL_INVALID_FD;
    }

    addr.sa_family = AOSL_AF_INET;
    addr.sa_port   = aosl_htons((uint16_t)port);

    aosl_fd_t fd = aosl_hal_sk_socket(AOSL_AF_INET, AOSL_SOCK_STREAM, AOSL_IPPROTO_TCP);
    if (aosl_fd_invalid(fd)) {
        return AOSL_INVALID_FD;
    }

    if (aosl_hal_sk_connect(fd, &addr) < 0) {
        aosl_hal_sk_close(fd);
        return AOSL_INVALID_FD;
    }

    return fd;
}

/*
 * Send all bytes (retry on short send).
 * Returns 0 on success, -1 on error.
 */
static int send_all(aosl_fd_t fd, const char *data, size_t len)
{
    while (len > 0) {
        int n = aosl_hal_sk_send(fd, data, len, 0);
        if (n < 0) {
            return -1;
        }
        data += n;
        len  -= (size_t)n;
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
static char *read_all(aosl_fd_t fd, size_t *out_len)
{
    size_t cap = RECV_BUF_SIZE;
    size_t len = 0;
    char  *buf = (char *)aosl_hal_malloc(cap);
    if (!buf) {
        return NULL;
    }

    uint64_t deadline = aosl_hal_get_tick_ms() + HTTP_TIMEOUT_MS;

    while (aosl_hal_get_tick_ms() < deadline) {
        int ret = aosl_hal_sk_recv(fd, buf + len, (int)(cap - len - 1), 0);
        if (ret > 0) {
            len += (size_t)ret;
            buf[len] = '\0';

            /* grow buffer if needed */
            if (cap - len < RECV_BUF_SIZE / 2) {
                cap *= 2;
                char *nb = (char *)aosl_hal_realloc(buf, cap);
                if (!nb) {
                    goto fail;
                }
                buf = nb;
            }
        } else if (ret == 0) {
            /* connection closed */
            break;
        } else {
            /* error or EAGAIN — give it a short wait */
            aosl_hal_msleep(10);
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
static int parse_status_line(const char *line)
{
    if (strncmp(line, "HTTP/1.", 7) != 0) {
        return 0;
    }
    line += 7;
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
 * Parse a complete HTTP response from raw data.
 * Returns the response struct (body will point into or be a copy from raw).
 */
static int parse_response(const char *raw, size_t raw_len, mybot_http_response_t *resp)
{
    memset(resp, 0, sizeof(*resp));

    const char *p = raw;
    const char *end = raw + raw_len;

    /* status line */
    const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
    if (!nl) { return -1; }
    resp->status_code = parse_status_line(p);
    p = nl + 1;

    /* skip CR if present */
    if (p < end && *p == '\r') { p++; }

    /* headers */
    size_t body_offset = 0;
    int content_length = -1;

    while (p < end) {
        nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        if (!nl) { break; }

        size_t hdr_len = (size_t)(nl - p);
        /* end of headers: empty line */
        if (hdr_len == 0 || (hdr_len == 1 && *p == '\r')) {
            p = nl + 1;
            if (p < end && *p == '\r') { p++; }
            body_offset = (size_t)(p - raw);
            break;
        }

        /* parse Content-Length */
        if (hdr_len > 16 &&
            (strncasecmp(p, "Content-Length:", 15) == 0 ||
             strncasecmp(p, "content-length:", 15) == 0)) {
            const char *val = p + 15;
            while (val < nl && *val == ' ') { val++; }
            content_length = 0;
            while (val < nl && *val >= '0' && *val <= '9') {
                content_length = content_length * 10 + (*val++ - '0');
            }
        }

        p = nl + 1;
        if (p < end && *p == '\r') { p++; }
    }

    /* body */
    if (body_offset < raw_len) {
        size_t avail = raw_len - body_offset;

        if (content_length >= 0) {
            resp->body_len = (size_t)content_length;
            if (resp->body_len > avail) {
                resp->body_len = avail;
            }
        } else {
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
    }

    return 0;
}

/* ----------------------------------------------------------
 * Internal: common request logic
 * ---------------------------------------------------------- */
static int http_request(const char *method, const char *url,
                        const char *content_type, const char *req_body,
                        const char *extra_headers,
                        mybot_http_response_t *resp)
{
    url_parts_t parts;
    if (parse_url(url, &parts) < 0) {
        return -1;
    }

    aosl_fd_t fd = tcp_connect(parts.host, parts.port);
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
            "%s"            /* extra headers inserted here */
            "\r\n"
            "%s",
            parts.path, parts.host,
            content_type ? content_type : "application/octet-stream",
            strlen(req_body),
            extra_headers ? extra_headers : "",
            req_body);
    } else {
        req_len = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Connection: close\r\n"
            "%s"            /* extra headers inserted here */
            "\r\n",
            parts.path, parts.host,
            extra_headers ? extra_headers : "");
    }

    if (req_len < 0 || (size_t)req_len >= sizeof(req)) {
        aosl_hal_sk_close(fd);
        return -1;
    }

    /* Send request */
    int ret = send_all(fd, req, (size_t)req_len);
    if (ret < 0) {
        aosl_hal_sk_close(fd);
        return -1;
    }

    /* Read response */
    size_t raw_len = 0;
    char *raw = read_all(fd, &raw_len);
    aosl_hal_sk_close(fd);

    if (!raw) {
        return -1;
    }

    /* Parse */
    ret = parse_response(raw, raw_len, resp);
    aosl_hal_free(raw);

    return ret;
}

/* ----------------------------------------------------------
 * Public API
 * ---------------------------------------------------------- */

int mybot_http_get(const char *url, mybot_http_response_t *resp)
{
    return mybot_http_get_ex(url, NULL, resp);
}

int mybot_http_post(const char *url, const char *content_type,
                    const char *body, mybot_http_response_t *resp)
{
    return mybot_http_post_ex(url, content_type, body, NULL, resp);
}

int mybot_http_get_ex(const char *url, const char *extra_headers, mybot_http_response_t *resp)
{
    if (!url || !resp) {
        return -1;
    }
    return http_request("GET", url, NULL, NULL, extra_headers, resp);
}

int mybot_http_post_ex(const char *url, const char *content_type, const char *body,
                       const char *extra_headers, mybot_http_response_t *resp)
{
    if (!url || !resp) {
        return -1;
    }
    return http_request("POST", url, content_type, body, extra_headers, resp);
}

void mybot_http_response_free(mybot_http_response_t *resp)
{
    if (resp) {
        if (resp->body) {
            aosl_hal_free(resp->body);
            resp->body = NULL;
        }
        resp->body_len = 0;
        resp->status_code = 0;
    }
}
