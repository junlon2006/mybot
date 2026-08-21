/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_https_bk725x.h"

#include <common/bk_err.h>
#include "mybot_platform_log.h"
#include <os/mem.h>
#include <os/os.h>

#include <lwip/api.h>
#include <lwip/err.h>
#include <lwip/ip_addr.h>
#include <lwip/sockets.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TAG "mybot_https"
#define DNS_THREAD_PRIORITY 2
#define DNS_THREAD_STACK_SIZE 4096

typedef struct {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
} bk725x_https_connection_t;

typedef struct {
    beken_mutex_t lock;
    beken_semaphore_t done;
    beken_thread_t worker;
    ip_addr_t address;
    err_t error;
    bool finished;
    bool caller_attached;
    char host[];
} bk725x_dns_request_t;

static uint32_t tick_ms(void) {
    return (uint32_t)rtos_get_time();
}

static int deadline_remaining_ms(uint32_t start, uint32_t timeout_ms) {
    uint32_t elapsed = tick_ms() - start;
    if (elapsed >= timeout_ms) {
        return 0;
    }
    return (int)(timeout_ms - elapsed);
}

static void log_mbedtls_error(const char *operation, int error) {
    char description[96];
    mbedtls_strerror(error, description, sizeof(description));
    MYBOT_LOGE(TAG, "%s failed (-0x%04x: %s)", operation,
            (unsigned int)(error < 0 ? -error : error), description);
}

static void dns_request_destroy(bk725x_dns_request_t *request) {
    if (request->done) {
        rtos_deinit_semaphore(&request->done);
    }
    if (request->lock) {
        rtos_deinit_mutex(&request->lock);
    }
    psram_free(request);
}

static void dns_worker(beken_thread_arg_t arg) {
    bk725x_dns_request_t *request = arg;
    ip_addr_t address;
    err_t error = netconn_gethostbyname_addrtype(request->host, &address,
                                                 NETCONN_DNS_IPV4_IPV6);
    bool orphaned = false;

    if (rtos_lock_mutex(&request->lock) == BK_OK) {
        request->error = error;
        if (error == ERR_OK) {
            request->address = address;
        }
        request->finished = true;
        orphaned = !request->caller_attached;
        rtos_unlock_mutex(&request->lock);
    } else {
        MYBOT_LOGE(TAG, "DNS worker failed to lock request state");
    }

    rtos_set_semaphore(&request->done);
    if (orphaned) {
        dns_request_destroy(request);
    }
    rtos_delete_thread(NULL);
}

static int resolve_host(ip_addr_t *address, const char *host, uint32_t start,
                        uint32_t timeout_ms) {
    int remaining_ms = deadline_remaining_ms(start, timeout_ms);
    if (remaining_ms <= 0) {
        return -1;
    }

    size_t host_size = strlen(host) + 1;
    if (host_size == 0 || host_size > SIZE_MAX - sizeof(bk725x_dns_request_t)) {
        return -1;
    }

    bk725x_dns_request_t *request = psram_zalloc(sizeof(*request) + host_size);
    if (!request) {
        MYBOT_LOGE(TAG, "DNS request allocation failed, host=%s", host);
        return -1;
    }
    memcpy(request->host, host, host_size);
    request->caller_attached = true;

    if (rtos_init_mutex(&request->lock) != BK_OK ||
        rtos_init_semaphore(&request->done, 1) != BK_OK) {
        MYBOT_LOGE(TAG, "DNS request initialization failed, host=%s", host);
        dns_request_destroy(request);
        return -1;
    }
    if (rtos_create_psram_thread(&request->worker, DNS_THREAD_PRIORITY, "mybot_dns",
                                 dns_worker, DNS_THREAD_STACK_SIZE, request) != BK_OK) {
        MYBOT_LOGE(TAG, "DNS worker creation failed, host=%s", host);
        dns_request_destroy(request);
        return -1;
    }

    remaining_ms = deadline_remaining_ms(start, timeout_ms);
    bk_err_t wait_result = remaining_ms > 0
                               ? rtos_get_semaphore(&request->done, (uint32_t)remaining_ms)
                               : BK_ERR_TIMEOUT;
    if (rtos_lock_mutex(&request->lock) != BK_OK) {
        MYBOT_LOGE(TAG, "DNS result lock failed, host=%s", host);
        rtos_thread_join(&request->worker);
        dns_request_destroy(request);
        return -1;
    }

    bool finished = request->finished;
    err_t error = request->error;
    if (finished && error == ERR_OK) {
        *address = request->address;
    }
    request->caller_attached = false;
    rtos_unlock_mutex(&request->lock);

    if (!finished) {
        if (wait_result == BK_OK) {
            rtos_thread_join(&request->worker);
            dns_request_destroy(request);
            MYBOT_LOGE(TAG, "DNS worker returned without a result, host=%s", host);
            return -1;
        }
        MYBOT_LOGE(TAG, "DNS resolve timeout, host=%s elapsed=%u ms", host,
                (unsigned int)(tick_ms() - start));
        return -1;
    }

    rtos_thread_join(&request->worker);
    dns_request_destroy(request);
    if (wait_result != BK_OK || error != ERR_OK) {
        MYBOT_LOGE(TAG, "DNS resolve failed, host=%s error=%d elapsed=%u ms", host,
                (int)error, (unsigned int)(tick_ms() - start));
        return -1;
    }
    return 0;
}

static int wait_for_net(mbedtls_net_context *net, uint32_t event, uint32_t start,
                        uint32_t timeout_ms) {
    for (;;) {
        int remaining_ms = deadline_remaining_ms(start, timeout_ms);
        if (remaining_ms <= 0) {
            MYBOT_LOGE(TAG, "socket poll timeout, event=0x%x elapsed=%u ms",
                    (unsigned int)event, (unsigned int)(tick_ms() - start));
            return -1;
        }

        int ret = mbedtls_net_poll(net, event, (uint32_t)remaining_ms);
        if (ret < 0) {
            log_mbedtls_error("socket poll", ret);
            return -1;
        }
        if ((ret & (int)event) != 0) {
            return 0;
        }
        if (ret == 0) {
            MYBOT_LOGE(TAG, "socket poll timeout, event=0x%x elapsed=%u ms",
                    (unsigned int)event, (unsigned int)(tick_ms() - start));
            return -1;
        }
    }
}

static int connect_resolved_address(mbedtls_net_context *net, const struct sockaddr *address,
                                    socklen_t address_len, uint32_t start,
                                    uint32_t timeout_ms) {
    int fd = socket(address->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        MYBOT_LOGE(TAG, "TCP socket creation failed, family=%d errno=%d",
                (int)address->sa_family, errno);
        return -1;
    }

    net->fd = fd;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        MYBOT_LOGE(TAG, "failed to set TCP socket nonblocking, errno=%d", errno);
        mbedtls_net_free(net);
        return -1;
    }

    int ret = connect(fd, address, address_len);
    if (ret < 0 && errno != EINPROGRESS && errno != EAGAIN) {
        MYBOT_LOGE(TAG, "TCP connect request failed, errno=%d", errno);
        mbedtls_net_free(net);
        return -1;
    }

    if (ret < 0 && wait_for_net(net, MBEDTLS_NET_POLL_WRITE, start, timeout_ms) < 0) {
        mbedtls_net_free(net);
        return -1;
    }

    int socket_error = 0;
    socklen_t error_len = sizeof(socket_error);
    if (getsockopt(net->fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) != 0 ||
        socket_error != 0) {
        MYBOT_LOGE(TAG, "TCP socket error after connect, error=%d errno=%d", socket_error,
                errno);
        mbedtls_net_free(net);
        return -1;
    }

    return 0;
}

static int tcp_connect(mbedtls_net_context *net, const char *host, uint16_t port,
                       uint32_t start, uint32_t timeout_ms) {
    ip_addr_t address;
    if (resolve_host(&address, host, start, timeout_ms) < 0) {
        return -1;
    }
    if (deadline_remaining_ms(start, timeout_ms) <= 0) {
        MYBOT_LOGE(TAG, "TCP connect deadline exhausted after DNS, host=%s", host);
        return -1;
    }

    struct sockaddr_storage storage;
    memset(&storage, 0, sizeof(storage));
    socklen_t address_len;
    if (IP_IS_V4(&address)) {
        struct sockaddr_in *target = (struct sockaddr_in *)&storage;
        target->sin_family = AF_INET;
        target->sin_port = htons(port);
        target->sin_addr.s_addr = ip4_addr_get_u32(ip_2_ip4(&address));
        address_len = sizeof(*target);
    }
#if LWIP_IPV6
    else if (IP_IS_V6(&address)) {
        struct sockaddr_in6 *target = (struct sockaddr_in6 *)&storage;
        target->sin6_family = AF_INET6;
        target->sin6_port = htons(port);
        memcpy(&target->sin6_addr, ip_2_ip6(&address)->addr, sizeof(target->sin6_addr));
        target->sin6_scope_id = ip6_addr_zone(ip_2_ip6(&address));
        address_len = sizeof(*target);
    }
#endif
    else {
        MYBOT_LOGE(TAG, "DNS returned unsupported address type, host=%s", host);
        return -1;
    }

    return connect_resolved_address(net, (const struct sockaddr *)&storage, address_len, start,
                                    timeout_ms);
}

static int wait_for_tls_retry(bk725x_https_connection_t *connection, int tls_error,
                              uint32_t start, uint32_t timeout_ms) {
    if (tls_error == MBEDTLS_ERR_SSL_WANT_READ) {
        return wait_for_net(&connection->net, MBEDTLS_NET_POLL_READ, start, timeout_ms);
    }
    if (tls_error == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return wait_for_net(&connection->net, MBEDTLS_NET_POLL_WRITE, start, timeout_ms);
    }
    return -1;
}

static int tls_handshake(bk725x_https_connection_t *connection, uint32_t start,
                         uint32_t timeout_ms) {
    for (;;) {
        if (deadline_remaining_ms(start, timeout_ms) <= 0) {
            MYBOT_LOGE(TAG, "TLS handshake timeout, elapsed=%u ms",
                    (unsigned int)(tick_ms() - start));
            return -1;
        }

        int ret = mbedtls_ssl_handshake(&connection->ssl);
        if (ret == 0) {
            return 0;
        }
        if (wait_for_tls_retry(connection, ret, start, timeout_ms) < 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                log_mbedtls_error("TLS handshake", ret);
            }
            return -1;
        }
    }
}

static bool host_is_ip_literal(const char *host) {
    unsigned char address[16];
    return mbedtls_x509_crt_parse_cn_inet_pton(host, address) != 0;
}

void mybot_https_bk725x_close(void *opaque_connection) {
    bk725x_https_connection_t *connection = opaque_connection;
    if (!connection) {
        return;
    }

    if (connection->net.fd >= 0) {
        int ret = mbedtls_ssl_close_notify(&connection->ssl);
        if (ret < 0 && ret != MBEDTLS_ERR_SSL_WANT_READ &&
            ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            log_mbedtls_error("close_notify", ret);
        }
    }
    mbedtls_net_free(&connection->net);
    mbedtls_ssl_free(&connection->ssl);
    mbedtls_ssl_config_free(&connection->config);
    mbedtls_ctr_drbg_free(&connection->ctr_drbg);
    mbedtls_entropy_free(&connection->entropy);
    psram_free(connection);
}

int mybot_https_bk725x_connect(void **out_connection, const char *host, uint16_t port,
                               int timeout_ms) {
    if (!out_connection || !host || !host[0] || port == 0 || timeout_ms <= 0) {
        MYBOT_LOGE(TAG, "invalid connect request");
        return -1;
    }
    *out_connection = NULL;

    uint32_t start = tick_ms();
    uint32_t timeout = (uint32_t)timeout_ms;
    bk725x_https_connection_t *connection = psram_zalloc(sizeof(*connection));
    if (!connection) {
        MYBOT_LOGE(TAG, "connection allocation failed");
        return -1;
    }

    mbedtls_net_init(&connection->net);
    mbedtls_ssl_init(&connection->ssl);
    mbedtls_ssl_config_init(&connection->config);
    mbedtls_ctr_drbg_init(&connection->ctr_drbg);
    mbedtls_entropy_init(&connection->entropy);

    static const unsigned char personalization[] = "mybot-bk725x-https";
    int ret = mbedtls_ctr_drbg_seed(&connection->ctr_drbg, mbedtls_entropy_func,
                                    &connection->entropy, personalization,
                                    sizeof(personalization) - 1);
    if (ret != 0) {
        log_mbedtls_error("random seed", ret);
        goto fail;
    }

    ret = mbedtls_ssl_config_defaults(&connection->config, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        log_mbedtls_error("TLS configuration", ret);
        goto fail;
    }

    mbedtls_ssl_conf_min_tls_version(&connection->config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_authmode(&connection->config, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&connection->config, mbedtls_ctr_drbg_random,
                         &connection->ctr_drbg);

    ret = mbedtls_ssl_setup(&connection->ssl, &connection->config);
    if (ret != 0) {
        log_mbedtls_error("TLS setup", ret);
        goto fail;
    }

    bool ip_literal = host_is_ip_literal(host);
    if (!ip_literal) {
        ret = mbedtls_ssl_set_hostname(&connection->ssl, host);
        if (ret != 0) {
            log_mbedtls_error("TLS host name", ret);
            goto fail;
        }
    }

    if (deadline_remaining_ms(start, timeout) <= 0) {
        MYBOT_LOGE(TAG, "connect timeout before TCP, host=%s", host);
        goto fail;
    }
    if (tcp_connect(&connection->net, host, port, start, timeout) < 0) {
        goto fail;
    }

    mbedtls_ssl_set_bio(&connection->ssl, &connection->net, mbedtls_net_send,
                        mbedtls_net_recv, NULL);
    if (tls_handshake(connection, start, timeout) < 0) {
        goto fail;
    }

    *out_connection = connection;
    return 0;

fail:
    mybot_https_bk725x_close(connection);
    return -1;
}

int mybot_https_bk725x_send(void *opaque_connection, const void *data, size_t len,
                            int timeout_ms) {
    bk725x_https_connection_t *connection = opaque_connection;
    if (!connection || !data || len == 0 || timeout_ms <= 0) {
        MYBOT_LOGE(TAG, "invalid send request");
        return -1;
    }

    if (len > INT_MAX) {
        len = INT_MAX;
    }
    uint32_t start = tick_ms();
    uint32_t timeout = (uint32_t)timeout_ms;
    for (;;) {
        if (deadline_remaining_ms(start, timeout) <= 0) {
            MYBOT_LOGE(TAG, "send timeout, bytes=%lu elapsed=%u ms", (unsigned long)len,
                    (unsigned int)(tick_ms() - start));
            return -1;
        }

        int ret = mbedtls_ssl_write(&connection->ssl, data, len);
        if (ret > 0) {
            return ret;
        }
        if (wait_for_tls_retry(connection, ret, start, timeout) < 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                log_mbedtls_error("TLS send", ret);
            }
            return -1;
        }
    }
}

int mybot_https_bk725x_recv(void *opaque_connection, void *data, size_t capacity,
                            int timeout_ms) {
    bk725x_https_connection_t *connection = opaque_connection;
    if (!connection || !data || capacity == 0 || timeout_ms <= 0) {
        MYBOT_LOGE(TAG, "invalid receive request");
        return -1;
    }

    if (capacity > INT_MAX) {
        capacity = INT_MAX;
    }
    uint32_t start = tick_ms();
    uint32_t timeout = (uint32_t)timeout_ms;
    for (;;) {
        if (deadline_remaining_ms(start, timeout) <= 0) {
            MYBOT_LOGE(TAG, "receive timeout, capacity=%lu elapsed=%u ms",
                    (unsigned long)capacity, (unsigned int)(tick_ms() - start));
            return -1;
        }

        int ret = mbedtls_ssl_read(&connection->ssl, data, capacity);
        if (ret > 0) {
            return ret;
        }
        if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return 0;
        }
        if (wait_for_tls_retry(connection, ret, start, timeout) < 0) {
            if (ret != 0 && ret != MBEDTLS_ERR_SSL_WANT_READ &&
                ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                log_mbedtls_error("TLS receive", ret);
            }
            return -1;
        }
    }
}
