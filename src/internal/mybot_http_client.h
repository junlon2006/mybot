/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_HTTP_CLIENT_H_
#define MYBOT_HTTP_CLIENT_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** HTTP response returned after a syntactically valid response, including a
 * non-2xx status. Release owned body storage with mybot_http_client_response_free(). */
typedef struct {
    int status_code; /* HTTP status code (200, 404, etc.), 0 if parse failed */
    char *body;      /* response body (NUL-terminated), NULL if empty */
    size_t body_len; /* body length in bytes */
} mybot_http_client_response_t;

/**
 * @brief HTTP GET with extra custom headers.
 * @param extra_headers Additional header lines to append (e.g.
 *                      "Authorization: Bearer x\r\n"), or NULL
 * @param resp          [out] response data; release with
 *                      mybot_http_client_response_free()
 * @return 0 when a response was parsed, or -1 on transport, timeout, or parse error.
 */
int mybot_http_client_get_ex(const char *url, const char *extra_headers,
                             mybot_http_client_response_t *resp);

/**
 * @brief HTTP POST with extra custom headers.
 * @param extra_headers Additional header lines to append, or NULL
 * @param resp          [out] response data; release with
 *                      mybot_http_client_response_free()
 * @return 0 when a response was parsed, or -1 on transport, timeout, or parse error.
 */
int mybot_http_client_post_ex(const char *url, const char *content_type, const char *body,
                              const char *extra_headers, mybot_http_client_response_t *resp);

/**
 * @brief Free resources allocated in a response.
 */
void mybot_http_client_response_free(mybot_http_client_response_t *resp);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_HTTP_CLIENT_H_ */
