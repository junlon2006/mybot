// #define BOOL_DEFINE_CONFLICT
#include <hal/aosl_hal_socket.h>
#include <hal/aosl_hal_errno.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <lwip/inet.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

// Helper to convert domains
static inline int conv_domain_to_os(enum aosl_socket_domain domain)
{
	switch (domain) {
		case AOSL_AF_UNSPEC: return AF_UNSPEC;
		case AOSL_AF_INET: return AF_INET;
		case AOSL_AF_INET6: return AF_INET6;
		default: return AF_UNSPEC;
	}
}

static inline int conv_type_to_os(enum aosl_socket_type type)
{
	switch (type) {
		case AOSL_SOCK_STREAM: return SOCK_STREAM;
		case AOSL_SOCK_DGRAM: return SOCK_DGRAM;
		default: return -1;
	}
}

static inline int conv_proto_to_os(enum aosl_socket_proto proto)
{
	switch (proto) {
	case AOSL_IPPROTO_TCP: return IPPROTO_TCP;
	case AOSL_IPPROTO_UDP: return IPPROTO_UDP;
	case AOSL_IPPROTO_AUTO: return 0;
	default: return 0;
	}
}

static inline void conv_addr_to_os(const aosl_sockaddr_t *ah_addr, struct sockaddr *os_addr)
{
    if (!ah_addr || !os_addr) return;
    // Simplified for IPv4 for now, add IPv6 if needed and supported
    if (ah_addr->sa_family == AOSL_AF_INET) {
        struct sockaddr_in *v4 = (struct sockaddr_in *)os_addr;
        v4->sin_family = AF_INET;
        v4->sin_port = ah_addr->sa_port;
        v4->sin_addr.s_addr = ah_addr->sin_addr;
    }
}

static inline void conv_addr_to_aosl(const struct sockaddr *os_addr, aosl_sockaddr_t *ah_addr)
{
    if (!os_addr || !ah_addr) return;
    if (os_addr->sa_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)os_addr;
        ah_addr->sa_family = AOSL_AF_INET;
        ah_addr->sa_port = v4->sin_port;
        ah_addr->sin_addr = v4->sin_addr.s_addr;
    }
}

int aosl_hal_sk_socket(enum aosl_socket_domain domain,
					   enum aosl_socket_type type,
					   enum aosl_socket_proto protocol)
{
	int n_domain = conv_domain_to_os(domain);
	int n_type = conv_type_to_os(type);
	int n_proto = conv_proto_to_os(protocol);

	return socket(n_domain, n_type, n_proto);
}

int aosl_hal_sk_bind(int sockfd, const aosl_sockaddr_t* addr)
{
	struct sockaddr_in com_addr = {0};
	struct sockaddr *n_addr = (struct sockaddr *)&com_addr;
	conv_addr_to_os(addr, n_addr);
	socklen_t addrlen = sizeof(struct sockaddr_in);
	int ret = bind(sockfd, n_addr, addrlen);
	if (ret < 0) return aosl_hal_errno_convert(errno);
	return 0;
}

int aosl_hal_sk_bind_device(int sockfd, const char *if_name)
{
    // Not typically supported in LwIP socket API directly via SO_BINDTODEVICE
	return 0;
}

int aosl_hal_sk_listen(int sockfd, int backlog)
{
	int ret = listen(sockfd, backlog);
	if (ret < 0) return aosl_hal_errno_convert(errno);
	return 0;
}

int aosl_hal_sk_accept(int sockfd, aosl_sockaddr_t *addr)
{
	struct sockaddr_in com_addr = {0};
	struct sockaddr *n_addr = (struct sockaddr *)&com_addr;
	socklen_t addrlen = sizeof(com_addr);
	int ret = accept(sockfd, n_addr, &addrlen);
	if (ret < 0) {
		return aosl_hal_errno_convert(errno);
	} else {
		conv_addr_to_aosl(n_addr, addr);
	}
	return ret;
}

int aosl_hal_sk_connect(int sockfd, const aosl_sockaddr_t *addr)
{
	struct sockaddr_in com_addr = {0};
	struct sockaddr *n_addr = (struct sockaddr *)&com_addr;
	conv_addr_to_os(addr, n_addr);
	socklen_t addrlen = sizeof(com_addr);
	int ret = connect(sockfd, n_addr, addrlen);
	if (ret < 0) return aosl_hal_errno_convert(errno);
	return 0;
}

int aosl_hal_sk_close(int sockfd)
{
	return close(sockfd); // close or closesocket? usually close in lwip
}

int aosl_hal_sk_send(int sockfd, const void* buf, size_t len, int flags)
{
	int ret = send(sockfd, buf, len, flags);
	if (ret < 0) return aosl_hal_errno_convert(errno);
	return ret;
}

int aosl_hal_sk_recv(int sockfd, void* buf, size_t len, int flags)
{
	int ret = recv(sockfd, buf, len, flags);
	if (ret < 0) return aosl_hal_errno_convert(errno);
	return ret;
}

int aosl_hal_sk_sendto(int sockfd, const void *buffer, size_t length,
						int flags, const aosl_sockaddr_t *dest_addr)
{
	struct sockaddr_in com_addr = {0};
	struct sockaddr *n_dest_addr = (struct sockaddr *)&com_addr;
	conv_addr_to_os(dest_addr, n_dest_addr);
	socklen_t addrlen = sizeof(com_addr);
	int ret = sendto(sockfd, buffer, length, flags, n_dest_addr, addrlen);
	if (ret < 0) {
		printf("[DEBUG sendto] fd=%d errno=%d\n", sockfd, errno);
		return aosl_hal_errno_convert(errno);
	}
	return ret;
}

int aosl_hal_sk_recvfrom(int sockfd, void *buffer, size_t length,
						int flags, aosl_sockaddr_t *src_addr)
{
	struct sockaddr_in com_addr = {0};
	struct sockaddr *n_src_addr = (struct sockaddr *)&com_addr;
	socklen_t addrlen = sizeof(com_addr);
	int ret = recvfrom(sockfd, buffer, length, flags, n_src_addr, &addrlen);
	if (ret < 0) {
		ret = aosl_hal_errno_convert(errno);
	} else {
		conv_addr_to_aosl(n_src_addr, src_addr);
	}
	return ret;
}

int aosl_hal_sk_setsockopt(int sockfd, int level, int optname,
                            const void *optval, socklen_t optlen)
{
    int ret = setsockopt(sockfd, level, optname, optval, optlen);
    if (ret < 0) return aosl_hal_errno_convert(errno);
    return ret;
}

int aosl_hal_sk_set_dscp(aosl_fd_t sockfd, enum aosl_socket_domain domain, uint8_t dscp)
{
    (void)sockfd;
    (void)domain;
    (void)dscp;
    return 0;
}

#include "lwip.h"

int aosl_hal_sk_get_local_ip(aosl_sockaddr_t *addr)
{
    char ip_str[32] = {0};
    // Use WIFI_NETIF as default for AC79 IoT SDK
    Get_IPAddress(WIFI_NETIF, ip_str);
    if (ip_str[0] == '\0' || strcmp(ip_str, "0.0.0.0") == 0) {
        return -1;
    }
    addr->sa_family = AOSL_AF_INET;
    aosl_inet_addr_from_string(&addr->sin_addr, ip_str);
    return 0;
}

int aosl_hal_sk_get_sockname(int sockfd, aosl_sockaddr_t *addr)
{
	struct sockaddr_in com_addr = {0};
	struct sockaddr *n_addr = (struct sockaddr *)&com_addr;
	socklen_t addrlen = sizeof(com_addr);
	int err = getsockname(sockfd, n_addr, &addrlen);
	if (err == 0) {
		conv_addr_to_aosl(n_addr, addr);
	}
	return err;
}

#include "lwip/netdb.h"

int aosl_hal_gethostbyname(const char *hostname, aosl_sockaddr_t *addrs, int addr_count)
{
    struct hostent *hp = gethostbyname(hostname);
    if (!hp || hp->h_addrtype != AF_INET) {
        return 0;
    }

    int count = 0;
    for (int i = 0; hp->h_addr_list[i] != NULL && i < addr_count; i++) {
        addrs[i].sa_family = AOSL_AF_INET;
        memcpy(&addrs[i].sin_addr, hp->h_addr_list[i], hp->h_length);
        count++;
    }
    return count;
}

int aosl_hal_sk_set_nonblock(int sockfd)
{
    unsigned long mode = 1;
    return ioctlsocket(sockfd, FIONBIO, &mode);
}

int aosl_hal_sk_read(int sockfd, void *buf, size_t count)
{
    return aosl_hal_sk_recv(sockfd, buf, count, 0);
}

int aosl_hal_sk_write(int sockfd, const void *buf, size_t count)
{
    return aosl_hal_sk_send(sockfd, buf, count, 0);
}
