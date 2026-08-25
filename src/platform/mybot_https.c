/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_https.h>

#include "mybot_platform_registry.h"
#include "mybot_https_transport.h"

#include <stddef.h>

int mybot_https_transport_connect(void **connection, const char *host, uint16_t port,
                                  int timeout_ms) {
    const mybot_https_ops_t *ops = mybot_platform_registry_get()->https;
    if (!ops || !connection || !host || !host[0] || port == 0 || timeout_ms <= 0) {
        return -1;
    }
    *connection = NULL;
    int ret = ops->connect(connection, host, port, timeout_ms);
    if (ret < 0 && *connection) {
        ops->close(*connection);
        *connection = NULL;
    }
    return ret;
}

int mybot_https_transport_send(void *connection, const void *data, size_t len, int timeout_ms) {
    const mybot_https_ops_t *ops = mybot_platform_registry_get()->https;
    if (!ops || !connection || !data || len == 0 || timeout_ms <= 0) {
        return -1;
    }
    return ops->send(connection, data, len, timeout_ms);
}

int mybot_https_transport_recv(void *connection, void *data, size_t capacity, int timeout_ms) {
    const mybot_https_ops_t *ops = mybot_platform_registry_get()->https;
    if (!ops || !connection || !data || capacity == 0 || timeout_ms <= 0) {
        return -1;
    }
    return ops->recv(connection, data, capacity, timeout_ms);
}

void mybot_https_transport_close(void *connection) {
    const mybot_https_ops_t *ops = mybot_platform_registry_get()->https;
    if (ops && connection) {
        ops->close(connection);
    }
}
