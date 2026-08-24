/* SPDX-License-Identifier: Apache-2.0 */
/* getaddrinfo, poll, and socket options are POSIX-2001/2008 APIs that glibc
 * hides under strict -std=c99 unless a feature-test macro is defined. */
#define _POSIX_C_SOURCE 200112L

#include <mybot/mybot_build_config.h>
#include <mybot/platform/mybot_https.h>

#include "linux_platform_adapters.h"

#include <api/aosl_log.h>

#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct {
    SSL_CTX *ssl_ctx;
    SSL *ssl;
    int fd;
} linux_https_connection_t;

static int set_socket_timeout(int fd, int timeout_ms) {
    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
                   setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0
               ? 0
               : -1;
}

static int connect_socket(const char *host, uint16_t port, int timeout_ms) {
    char service[6];
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (snprintf(service, sizeof(service), "%u", (unsigned int)port) < 0 ||
        getaddrinfo(host, service, &hints, &addresses) != 0) {
        return -1;
    }

    int connected_fd = -1;
    for (const struct addrinfo *address = addresses; address; address = address->ai_next) {
        int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) {
            continue;
        }

        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd);
            continue;
        }

        int ret = connect(fd, address->ai_addr, address->ai_addrlen);
        if (ret < 0 && errno == EINPROGRESS) {
            struct pollfd poll_fd = {.fd = fd, .events = POLLOUT};
            do {
                ret = poll(&poll_fd, 1, timeout_ms);
            } while (ret < 0 && errno == EINTR);

            int socket_error = 0;
            socklen_t error_size = sizeof(socket_error);
            if (ret > 0 && (poll_fd.revents & POLLOUT) != 0 &&
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) == 0 &&
                socket_error == 0) {
                ret = 0;
            } else {
                ret = -1;
            }
        }

        if (ret == 0 && fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0 &&
            set_socket_timeout(fd, timeout_ms) == 0) {
            connected_fd = fd;
            break;
        }
        close(fd);
    }

    freeaddrinfo(addresses);
    return connected_fd;
}

static int configure_peer_name(SSL *ssl, const char *host) {
    unsigned char address[sizeof(struct in6_addr)];
    int is_ip = inet_pton(AF_INET, host, address) == 1 || inet_pton(AF_INET6, host, address) == 1;
    X509_VERIFY_PARAM *verify = SSL_get0_param(ssl);
    if (is_ip) {
        return X509_VERIFY_PARAM_set1_ip_asc(verify, host) == 1 ? 0 : -1;
    }
    if (SSL_set_tlsext_host_name(ssl, host) != 1) {
        return -1;
    }
    return SSL_set1_host(ssl, host) == 1 ? 0 : -1;
}

static int https_connect(void **out_connection, const char *host, uint16_t port, int timeout_ms) {
    linux_https_connection_t *connection = calloc(1, sizeof(*connection));
    if (!connection) {
        return -1;
    }
    connection->fd = -1;

    connection->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!connection->ssl_ctx ||
        SSL_CTX_set_min_proto_version(connection->ssl_ctx, TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_default_verify_paths(connection->ssl_ctx) != 1) {
        goto fail;
    }
    SSL_CTX_set_verify(connection->ssl_ctx, SSL_VERIFY_PEER, NULL);

    connection->fd = connect_socket(host, port, timeout_ms);
    if (connection->fd < 0) {
        goto fail;
    }

    connection->ssl = SSL_new(connection->ssl_ctx);
    if (!connection->ssl || configure_peer_name(connection->ssl, host) < 0 ||
        SSL_set_fd(connection->ssl, connection->fd) != 1 || SSL_connect(connection->ssl) != 1 ||
        SSL_get_verify_result(connection->ssl) != X509_V_OK) {
        goto fail;
    }

    *out_connection = connection;
    return 0;

fail:
    if (connection->ssl) {
        SSL_free(connection->ssl);
    }
    if (connection->ssl_ctx) {
        SSL_CTX_free(connection->ssl_ctx);
    }
    if (connection->fd >= 0) {
        close(connection->fd);
    }
    free(connection);
    return -1;
}

static int https_send(void *opaque, const void *data, size_t len, int timeout_ms) {
    linux_https_connection_t *connection = opaque;
    if (set_socket_timeout(connection->fd, timeout_ms) < 0) {
        return -1;
    }
    int request = len > INT_MAX ? INT_MAX : (int)len;
    int ret = SSL_write(connection->ssl, data, request);
    return ret > 0 ? ret : -1;
}

static int https_recv(void *opaque, void *data, size_t capacity, int timeout_ms) {
    linux_https_connection_t *connection = opaque;
    if (set_socket_timeout(connection->fd, timeout_ms) < 0) {
        return -1;
    }
    int request = capacity > INT_MAX ? INT_MAX : (int)capacity;
    int ret = SSL_read(connection->ssl, data, request);
    if (ret > 0) {
        return ret;
    }
    int error = SSL_get_error(connection->ssl, ret);
    return error == SSL_ERROR_ZERO_RETURN ? 0 : -1;
}

static void https_close(void *opaque) {
    linux_https_connection_t *connection = opaque;
    if (!connection) {
        return;
    }
    if (connection->ssl) {
        (void)SSL_shutdown(connection->ssl);
        SSL_free(connection->ssl);
    }
    if (connection->ssl_ctx) {
        SSL_CTX_free(connection->ssl_ctx);
    }
    if (connection->fd >= 0) {
        close(connection->fd);
    }
    free(connection);
}

static const mybot_https_ops_t s_https_ops = {
    .connect = https_connect,
    .send = https_send,
    .recv = https_recv,
    .close = https_close,
};

const mybot_https_ops_t *linux_https_platform_openssl_ops(void) {
    return &s_https_ops;
}
