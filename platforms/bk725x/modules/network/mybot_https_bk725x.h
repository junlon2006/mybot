/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_HTTPS_BK725X_H_
#define MYBOT_HTTPS_BK725X_H_

#include <stddef.h>
#include <stdint.h>

int mybot_https_bk725x_connect(void **connection, const char *host, uint16_t port,
                               int timeout_ms);
int mybot_https_bk725x_send(void *connection, const void *data, size_t len,
                            int timeout_ms);
int mybot_https_bk725x_recv(void *connection, void *data, size_t capacity,
                            int timeout_ms);
void mybot_https_bk725x_close(void *connection);

#endif /* MYBOT_HTTPS_BK725X_H_ */
