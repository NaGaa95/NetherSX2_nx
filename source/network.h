#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>

struct bionic_sockaddr {
  uint16_t family;
  unsigned char data[14];
};

struct bionic_ifaddrs {
  struct bionic_ifaddrs *next;
  char *name;
  unsigned flags;
  struct bionic_sockaddr *address, *netmask, *broadcast;
  void *data;
};

struct bionic_msghdr {
  void *name;
  uint32_t name_length;
  struct iovec *iov;
  size_t iov_count;
  void *control;
  size_t control_length;
  int flags;
};

struct bionic_addrinfo {
  int flags, family, socktype, protocol;
  uint32_t address_length;
  char *canonical_name;
  struct bionic_sockaddr *address;
  struct bionic_addrinfo *next;
};

typedef struct {
  uint32_t address, netmask, gateway, dns[2];
} NetConfig;

extern volatile int g_net_ready;

int net_read_config(NetConfig *config);
int net_getifaddrs(struct bionic_ifaddrs **out);
void net_freeifaddrs(struct bionic_ifaddrs *list);
FILE *net_open_config(const char *path);
int net_ioctl(int fd, unsigned long request, void *argument);

int net_socket(int family, int type, int protocol);
int net_bind(int fd, const struct bionic_sockaddr *address, uint32_t length);
int net_connect(int fd, const struct bionic_sockaddr *address, uint32_t length);
ssize_t net_sendto(int fd, const void *buffer, size_t size, int flags,
                   const struct bionic_sockaddr *address, uint32_t length);
ssize_t net_recvfrom(int fd, void *buffer, size_t size, int flags,
                     struct bionic_sockaddr *address, uint32_t *length);
ssize_t net_recvmsg(int fd, struct bionic_msghdr *message, int flags);
int net_select(int count, fd_set *read, fd_set *write, fd_set *error, struct timeval *timeout);
int net_setsockopt(int fd, int level, int option, const void *value, uint32_t length);
int net_getsockopt(int fd, int level, int option, void *value, uint32_t *length);
int net_getsockname(int fd, struct bionic_sockaddr *address, uint32_t *length);
int net_shutdown(int fd, int how);
int net_getaddrinfo(const char *node, const char *service,
                    const struct bionic_addrinfo *hints, struct bionic_addrinfo **result);
void net_freeaddrinfo(struct bionic_addrinfo *list);
int net_inet_pton(int family, const char *text, void *address);
