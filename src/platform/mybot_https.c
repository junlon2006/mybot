/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_https.h>

#include "mybot_https_internal.h"
#include "mybot_https_transport.h"

#include <stddef.h>

static const mybot_https_ops_t *s_ops;

int mybot_https_register(const mybot_https_ops_t *ops) {
    if (!ops || !ops->name || !ops->name[0] || !ops->connect || !ops->send || !ops->recv ||
        !ops->close || s_ops) {
        return -1;
    }
    s_ops = ops;
    return 0;
}

bool mybot_https_is_registered(void) {
    return s_ops != NULL;
}

int mybot_https_transport_connect(void **connection, const char *host, uint16_t port,
                                  int timeout_ms) {
    if (!s_ops || !connection || !host || !host[0] || port == 0 || timeout_ms <= 0) {
        return -1;
    }
    *connection = NULL;
    int ret = s_ops->connect(connection, host, port, timeout_ms);
    if (ret < 0 && *connection) {
        s_ops->close(*connection);
        *connection = NULL;
    }
    return ret;
}

int mybot_https_transport_send(void *connection, const void *data, size_t len, int timeout_ms) {
    if (!s_ops || !connection || !data || len == 0 || timeout_ms <= 0) {
        return -1;
    }
    return s_ops->send(connection, data, len, timeout_ms);
}

int mybot_https_transport_recv(void *connection, void *data, size_t capacity, int timeout_ms) {
    if (!s_ops || !connection || !data || capacity == 0 || timeout_ms <= 0) {
        return -1;
    }
    return s_ops->recv(connection, data, capacity, timeout_ms);
}

void mybot_https_transport_close(void *connection) {
    if (s_ops && connection) {
        s_ops->close(connection);
    }
}
