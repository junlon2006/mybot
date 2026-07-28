#ifndef HTTP_CLIENT_H_
#define HTTP_CLIENT_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** HTTP response returned by http_get / http_post. */
typedef struct {
    int   status_code;   /* HTTP status code (200, 404, etc.), 0 if parse failed */
    char *body;          /* response body (null-terminated), NULL if empty */
    size_t body_len;     /* body length in bytes */
} http_response_t;

/**
 * @brief Simple blocking HTTP GET request.
 * @param url   URL in format "http://hostname[:port]/path"
 * @param resp  [out] response data (must call http_response_free when done)
 * @return 0 on success, -1 on error.
 */
int http_get(const char *url, http_response_t *resp);

/**
 * @brief Simple blocking HTTP POST request.
 * @param url           URL in format "http://hostname[:port]/path"
 * @param content_type  Content-Type header value (e.g. "application/json")
 * @param body          POST body data
 * @param resp          [out] response data
 * @return 0 on success, -1 on error.
 */
int http_post(const char *url, const char *content_type,
              const char *body, http_response_t *resp);

/**
 * @brief Free resources allocated in a response.
 */
void http_response_free(http_response_t *resp);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_CLIENT_H_ */
