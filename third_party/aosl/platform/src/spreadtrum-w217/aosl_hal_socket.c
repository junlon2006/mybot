#include <DAPS/export/inc/tcpip6/socket_types.h>
#include <DAPS/export/inc/tcpip6/socket_api.h>
#include <DAPS/export/inc/in_message.h>
#include <RTOS/export/inc/os_api.h>

#include <hal/aosl_hal_errno.h>
#include <hal/aosl_hal_socket.h>
#include <api/aosl_route.h>
#include <api/aosl_log.h>

/* ------------------------------------------------------------------ */
/*  Helper: get spreadtrum network ID                                 */
/* ------------------------------------------------------------------ */
static int get_spreadtrun_net_id(void)
{
  extern uint32_t agora_spreadtrun_net_id;
  return agora_spreadtrun_net_id;
}

/* ------------------------------------------------------------------ */
/*  Domain / type / protocol conversion helpers                        */
/* ------------------------------------------------------------------ */
static inline int conv_domain_to_sci(enum aosl_socket_domain domain)
{
  switch (domain) {
    case AOSL_AF_UNSPEC: return AF_UNSPEC;
    case AOSL_AF_INET:   return AF_INET;
    case AOSL_AF_INET6:  return AF_INET6;
    default:             return AF_UNSPEC;
  }
}

static inline int conv_type_to_sci(enum aosl_socket_type type)
{
  switch (type) {
    case AOSL_SOCK_STREAM: return SOCK_STREAM;
    case AOSL_SOCK_DGRAM:  return SOCK_DGRAM;
    default:               return -1;
  }
}

static inline int conv_proto_to_sci(enum aosl_socket_proto proto)
{
  (void)proto;
  return 0;   /* SCI API: 0 = auto-select */
}

/* ------------------------------------------------------------------ */
/*  Address conversion helpers                                         */
/*                                                                     */
/*  SCI uses struct sci_sockaddrext universally.  For IPv4 the first   */
/*  two bytes are AF_INET + port, then 4 bytes IP;  for IPv6 the       */
/*  sa_data area contains a full struct sci_sockaddr6.                 */
/* ------------------------------------------------------------------ */

/* aosl_sockaddr_t -> sci_sockaddrext */
static inline void conv_addr_to_sci(const aosl_sockaddr_t *ah_addr,
                                    struct sci_sockaddrext *ext)
{
  memset(ext, 0, sizeof(*ext));

  switch (ah_addr->sa_family) {
    case AOSL_AF_INET: {
      struct sci_sockaddr *sci = (struct sci_sockaddr *)ext;
      sci->family = AF_INET;
      sci->port   = ah_addr->sa_port;
      sci->ip_addr = ah_addr->sin_addr;
      break;
    }
    case AOSL_AF_INET6: {
      ext->sa_family = AF_INET6;
      {
        struct sci_sockaddr6 *v6 = (struct sci_sockaddr6 *)ext;
        v6->sin6_family   = AF_INET6;
        v6->sin6_port     = ah_addr->sa_port;
        v6->sin6_flowinfo = ah_addr->sin6_flowinfo;
        v6->sin6_scope_id = ah_addr->sin6_scope_id;
        memcpy(&v6->sin6_addr, &ah_addr->sin6_addr, 16);
      }
      break;
    }
    default:
      break;
  }
}

/* sci_sockaddrext -> aosl_sockaddr_t */
static inline void conv_addr_to_aosl(const struct sci_sockaddrext *ext,
                                     aosl_sockaddr_t *ah_addr)
{
  memset(ah_addr, 0, sizeof(*ah_addr));

  switch (ext->sa_family) {
    case AF_INET: {
      const struct sci_sockaddr *sci = (const struct sci_sockaddr *)ext;
      ah_addr->sa_family = AOSL_AF_INET;
      ah_addr->sa_port   = sci->port;
      ah_addr->sin_addr  = sci->ip_addr;
      break;
    }
    case AF_INET6: {
      const struct sci_sockaddr6 *v6 = (const struct sci_sockaddr6 *)ext;
      ah_addr->sa_family = AOSL_AF_INET6;
      ah_addr->sa_port   = v6->sin6_port;
      ah_addr->sin6_flowinfo = v6->sin6_flowinfo;
      ah_addr->sin6_scope_id = v6->sin6_scope_id;
      memcpy(&ah_addr->sin6_addr, &v6->sin6_addr, 16);
      break;
    }
    default:
      break;
  }
}

/* Return the SCI address size for a given sa_family */
static inline int sci_addr_size(uint16_t sa_family)
{
  switch (sa_family) {
    case AOSL_AF_INET6:
      return sizeof(struct sci_sockaddrext);
    default:
      return sizeof(struct sci_sockaddr);   /* covers IPv4 */
  }
}

/* ------------------------------------------------------------------ */
/*  HAL API implementation                                             */
/* ------------------------------------------------------------------ */
aosl_fd_t aosl_hal_sk_socket(enum aosl_socket_domain domain,
                             enum aosl_socket_type type,
                             enum aosl_socket_proto protocol)
{
  int sci_domain = conv_domain_to_sci(domain);
  int sci_type   = conv_type_to_sci(type);
  int fd = sci_sock_socket(sci_domain, sci_type, 0, get_spreadtrun_net_id());
  return (aosl_fd_t)fd;
}

int aosl_hal_sk_bind(int sockfd, const aosl_sockaddr_t *addr)
{
  struct sci_sockaddrext ext = {0};
  conv_addr_to_sci(addr, &ext);
  int ret = sci_sock_bind(sockfd, &ext, sci_addr_size(addr->sa_family));
  if (ret < 0) {
    return aosl_hal_errno_convert(sci_sock_errno(sockfd));
  }
  return 0;
}

int aosl_hal_sk_listen(int sockfd, int backlog)
{
  int ret = sci_sock_listen(sockfd, backlog);
  if (ret < 0) {
    return aosl_hal_errno_convert(sci_sock_errno(sockfd));
  }
  return 0;
}

aosl_fd_t aosl_hal_sk_accept(aosl_fd_t sockfd, aosl_sockaddr_t *addr)
{
  struct sci_sockaddrext ext = {0};
  int addr_len = sizeof(ext);
  int ret = sci_sock_accept(sockfd, &ext, &addr_len);
  if (ret < 0) {
    (void)aosl_hal_errno_convert(sci_sock_errno(sockfd));
    return AOSL_INVALID_FD;
  }
  if (addr) {
    conv_addr_to_aosl(&ext, addr);
  }
  return (aosl_fd_t)ret;
}

int aosl_hal_sk_connect(int sockfd, const aosl_sockaddr_t *addr)
{
  struct sci_sockaddrext ext = {0};
  conv_addr_to_sci(addr, &ext);
  int ret = sci_sock_connect(sockfd, &ext, sci_addr_size(addr->sa_family));
  if (ret < 0) {
    return aosl_hal_errno_convert(sci_sock_errno(sockfd));
  }
  return 0;
}

int aosl_hal_sk_close(int sockfd)
{
  return sci_sock_socketclose(sockfd);
}

int aosl_hal_sk_send(int sockfd, const void *buf, size_t len, int flags)
{
  int ret = sci_sock_send(sockfd, (char *)buf, (int)len, flags);
  if (ret < 0) {
    return aosl_hal_errno_convert(sci_sock_errno(sockfd));
  }
  return ret;
}

int aosl_hal_sk_recv(int sockfd, void *buf, size_t len, int flags)
{
  int ret = sci_sock_recv(sockfd, (char *)buf, (int)len, flags);
  if (ret < 0) {
    return aosl_hal_errno_convert(sci_sock_errno(sockfd));
  }
  return ret;
}

int aosl_hal_sk_sendto(int sockfd, const void *buffer, size_t length,
                       int flags, const aosl_sockaddr_t *dest_addr)
{
  struct sci_sockaddrext ext = {0};
  conv_addr_to_sci(dest_addr, &ext);
  int ret = sci_sock_sendto(sockfd, (char *)buffer, (int)length,
                            flags, &ext);
  if (ret < 0) {
    return aosl_hal_errno_convert(sci_sock_errno(sockfd));
  }
  return ret;
}

int aosl_hal_sk_recvfrom(int sockfd, void *buffer, size_t length,
                         int flags, aosl_sockaddr_t *src_addr)
{
  struct sci_sockaddrext ext = {0};
  int ret = sci_sock_recvfrom(sockfd, (char *)buffer, (int)length,
                              flags, &ext);
  if (ret < 0) {
    return aosl_hal_errno_convert(sci_sock_errno(sockfd));
  }
  if (src_addr) {
    conv_addr_to_aosl(&ext, src_addr);
  }
  return ret;
}

int aosl_hal_sk_read(int sockfd, void *buf, size_t count)
{
  int ret = sci_sock_recv(sockfd, (char *)buf, (int)count, 0);
  if (ret < 0) {
    return aosl_hal_errno_convert(sci_sock_errno(sockfd));
  }
  return ret;
}

int aosl_hal_sk_write(int sockfd, const void *buf, size_t count)
{
  int ret = sci_sock_send(sockfd, (char *)buf, (int)count, 0);
  if (ret < 0) {
    return aosl_hal_errno_convert(sci_sock_errno(sockfd));
  }
  return ret;
}

int aosl_hal_sk_set_nonblock(int sockfd)
{
  int ret = sci_sock_setsockopt(sockfd, SO_NBIO, NULL);
  if (ret < 0) {
    return -1;
  }
  return 0;
}

int aosl_hal_sk_get_local_ip(aosl_sockaddr_t *addr)
{
  (void)addr;
  return -1;
}

int aosl_hal_sk_get_sockname(int sockfd, aosl_sockaddr_t *addr)
{
  struct sci_sockaddrext ext = {0};
  int err = sci_sock_getsockname(sockfd, &ext);
  if (err == 0) {
    conv_addr_to_aosl(&ext, addr);
  }
  return err;
}

int aosl_hal_sk_bind_device(int sockfd, const char *if_name)
{
  (void)sockfd;
  (void)if_name;
  return 0;
}

int aosl_hal_sk_set_dscp(aosl_fd_t sockfd, enum aosl_socket_domain domain,
                         uint8_t dscp)
{
  (void)sockfd;
  (void)domain;
  (void)dscp;
  return 0;
}

int aosl_hal_gethostbyname(const char *hostname, aosl_sockaddr_t *addrs, int addr_count)
{
  struct sci_hostent *hostent;
  int count = 0;
  int i;
  ASYNC_GETHOSTBYNAME_CNF_SIG_T* dns_sig_ptr;

  if (NULL == hostname || NULL == addrs || addr_count <= 0) {
    return count;
  }

  sci_async_gethostbyname_ext(hostname, SCI_IdentifyThread(), 5000, AF_UNSPEC, get_spreadtrun_net_id());
  dns_sig_ptr = (ASYNC_GETHOSTBYNAME_CNF_SIG_T *)SCI_GetSignal(SCI_IdentifyThread());
  if (dns_sig_ptr->SignalCode != SOCKET_ASYNC_GETHOSTBYNAME_CNF) {
    return count;
  }

  hostent = &dns_sig_ptr->hostent;
  if (NULL == hostent) {
    return count;
  }

  /*
   * Spreadtrum sci_hostent stores IPv4/v6 addresses in separate arrays
   * with explicit counts (h_cntv4 / h_cntv6).  Iterate each array by
   * count rather than by null-termination (which may overrun).
   */

  /* IPv4: h_addr_list[i], i = 0 .. h_cntv4-1 */
  for (i = 0; i < hostent->h_cntv4 && count < addr_count; i++) {
    if (hostent->h_addr_list == NULL || hostent->h_addr_list[i] == NULL) {
      break;
    }
    aosl_sockaddr_t *addr = &addrs[count];
    addr->sa_family = AOSL_AF_INET;
    addr->sa_port = 0;
    addr->sin_addr = *(uint32_t *)hostent->h_addr_list[i];
    count++;
  }

  /* IPv6: h_addr6_list[i], i = 0 .. h_cntv6-1 */
  for (i = 0; i < hostent->h_cntv6 && count < addr_count; i++) {
    if (hostent->h_addr6_list == NULL || hostent->h_addr6_list[i] == NULL) {
      break;
    }
    aosl_sockaddr_t *addr = &addrs[count];
    addr->sa_family = AOSL_AF_INET6;
    addr->sa_port = 0;
    addr->sin6_flowinfo = 0;
    addr->sin6_scope_id = 0;
    memcpy(&addr->sin6_addr, hostent->h_addr6_list[i], 16);
    count++;
  }

  return count;
}
