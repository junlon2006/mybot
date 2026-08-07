#ifndef MYBOT_HTTPS_TRANSPORT_H_
#define MYBOT_HTTPS_TRANSPORT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * TLS stream operations used by the built-in HTTPS client.
 *
 * connect() must validate the server certificate chain and verify host against
 * the certificate. DNS hosts must also be sent as the TLS SNI name. send() and
 * recv() return a positive byte count on progress, recv() returns 0 when the
 * peer closes cleanly, and either function returns -1 on error or timeout.
 * timeout_ms is the maximum blocking time for that individual call.
 */
typedef struct {
    const char *name;
    int (*connect)(void **connection, const char *host, uint16_t port, int timeout_ms);
    int (*send)(void *connection, const void *data, size_t len, int timeout_ms);
    int (*recv)(void *connection, void *data, size_t capacity, int timeout_ms);
    void (*close)(void *connection);
} mybot_https_transport_ops_t;

/**
 * Register one platform TLS transport before mybot_app_start().
 * The ops table must remain valid for the process lifetime.
 */
int mybot_https_transport_register(const mybot_https_transport_ops_t *ops);

/** Return whether a platform TLS transport has been registered. */
bool mybot_https_transport_is_registered(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_HTTPS_TRANSPORT_H_ */
