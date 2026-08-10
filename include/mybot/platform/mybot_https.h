/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_HTTPS_H_
#define MYBOT_HTTPS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <mybot/mybot_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * TLS stream operations used by the built-in HTTPS client.
 *
 * The implementation wraps the platform TLS stack (e.g. mbedTLS, BearSSL or a
 * chipset TLS socket API). The SDK core does not link OpenSSL.
 */
typedef struct {
    /** Implementation name for logging and diagnostics. */
    const char *name;

    /**
     * Establish TCP and TLS to the server.
     *
     * Must validate the server certificate chain and verify the host against
     * the certificate. The DNS host must also be sent as the TLS SNI name.
     *
     * @param connection [out] TLS connection handle
     * @param host       NUL-terminated DNS host name
     * @param port       TCP port in host byte order
     * @param timeout_ms maximum blocking time for the whole operation
     * @return 0 on success, -1 on error or timeout
     */
    int (*connect)(void **connection, const char *host, uint16_t port, int timeout_ms);

    /**
     * Send bytes over the TLS stream.
     *
     * @param connection TLS connection handle from connect()
     * @param data       bytes to send
     * @param len        number of bytes to send
     * @param timeout_ms maximum blocking time for this call
     * @return positive byte count on progress, -1 on error or timeout
     */
    int (*send)(void *connection, const void *data, size_t len, int timeout_ms);

    /**
     * Receive bytes from the TLS stream.
     *
     * @param connection TLS connection handle from connect()
     * @param data       destination buffer
     * @param capacity   size of data in bytes
     * @param timeout_ms maximum blocking time for this call
     * @return positive byte count on progress, 0 when the peer closes
     *         cleanly, -1 on error or timeout
     */
    int (*recv)(void *connection, void *data, size_t capacity, int timeout_ms);

    /**
     * Close the TLS connection and release all resources.
     *
     * @param connection TLS connection handle from connect()
     */
    void (*close)(void *connection);
} mybot_https_ops_t;

/**
 * Register one platform TLS transport.
 *
 * @param ops TLS operations table; must remain valid for the process
 *            lifetime
 * @return 0 on success, -1 if ops is invalid or already registered
 *
 * @note Call exactly once, before mybot_start(). Do not disable certificate
 *       or hostname verification for development certificates; install the
 *       required CA in the device trust store instead.
 */
MYBOT_API int mybot_https_register(const mybot_https_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_HTTPS_H_ */
