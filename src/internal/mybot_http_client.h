#ifndef MYBOT_HTTP_CLIENT_H_
#define MYBOT_HTTP_CLIENT_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** HTTP response returned by mybot_http_client_get / mybot_http_client_post. */
typedef struct {
    int status_code; /* HTTP status code (200, 404, etc.), 0 if parse failed */
    char *body;      /* response body (null-terminated), NULL if empty */
    size_t body_len; /* body length in bytes */
} mybot_http_client_response_t;

/**
 * @brief Simple blocking HTTP GET request.
 * @param url   URL in format "http://hostname[:port]/path"
 * @param resp  [out] response data (must call mybot_http_client_response_free when done)
 * @return 0 on success, -1 on error.
 */
int mybot_http_client_get(const char *url, mybot_http_client_response_t *resp);

/**
 * @brief Simple blocking HTTP POST request.
 * @param url           URL in format "http://hostname[:port]/path"
 * @param content_type  Content-Type header value (e.g. "application/json")
 * @param body          POST body data
 * @param resp          [out] response data
 * @return 0 on success, -1 on error.
 */
int mybot_http_client_post(const char *url, const char *content_type, const char *body,
                           mybot_http_client_response_t *resp);

/**
 * @brief HTTP GET with extra custom headers.
 * @param extra_headers  Additional header lines to append (e.g. "Authorization: Bearer x\r\n"), or
 * NULL.
 */
int mybot_http_client_get_ex(const char *url, const char *extra_headers,
                             mybot_http_client_response_t *resp);

/**
 * @brief HTTP POST with extra custom headers.
 * @param extra_headers  Additional header lines to append, or NULL.
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
