/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_HTTPS_TRANSPORT_INTERNAL_H_
#define MYBOT_HTTPS_TRANSPORT_INTERNAL_H_

#include <stddef.h>
#include <stdint.h>

int mybot_https_transport_connect(void **connection, const char *host, uint16_t port,
                                  int timeout_ms);
int mybot_https_transport_send(void *connection, const void *data, size_t len, int timeout_ms);
int mybot_https_transport_recv(void *connection, void *data, size_t capacity, int timeout_ms);
void mybot_https_transport_close(void *connection);

#endif /* MYBOT_HTTPS_TRANSPORT_INTERNAL_H_ */
