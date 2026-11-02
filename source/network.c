#include "network.h"
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __SWITCH__
#include <sys/filio.h>
#endif

volatile int g_net_ready;

_Static_assert(sizeof(struct bionic_ifaddrs) == 56, "Android ifaddrs ABI");
_Static_assert(offsetof(struct bionic_ifaddrs, address) == 24, "Android ifaddrs address ABI");
_Static_assert(sizeof(struct bionic_msghdr) == 56, "Android msghdr ABI");
_Static_assert(offsetof(struct bionic_addrinfo, address) == 32, "Android addrinfo ABI");

static int bionic_errno(int error) {
  static const int errors[][2] = {
    {ENOSYS, 38}, {EOVERFLOW, 75}, {ENOTSOCK, 88}, {EDESTADDRREQ, 89},
    {EMSGSIZE, 90}, {EPROTOTYPE, 91}, {ENOPROTOOPT, 92}, {EPROTONOSUPPORT, 93},
#ifdef ESOCKTNOSUPPORT
    {ESOCKTNOSUPPORT, 94},
#endif
    {EOPNOTSUPP, 95}, {EAFNOSUPPORT, 97},
    {EADDRINUSE, 98}, {EADDRNOTAVAIL, 99}, {ENETDOWN, 100}, {ENETUNREACH, 101},
    {ENETRESET, 102}, {ECONNABORTED, 103}, {ECONNRESET, 104}, {ENOBUFS, 105},
    {EISCONN, 106}, {ENOTCONN, 107},
#ifdef ESHUTDOWN
    {ESHUTDOWN, 108},
#endif
    {ETOOMANYREFS, 109},
    {ETIMEDOUT, 110}, {ECONNREFUSED, 111}, {EHOSTDOWN, 112}, {EHOSTUNREACH, 113},
    {EALREADY, 114}, {EINPROGRESS, 115}, {ECANCELED, 125}
  };
  for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i)
    if (error == errors[i][0])
      return errors[i][1];
  return error;
}

static ssize_t socket_result(ssize_t result) {
  if (result < 0)
    errno = bionic_errno(errno);
  return result;
}

#define NET_GUARD do { if (!g_net_ready) { errno = 100; return -1; } } while (0)

static int current_config(NetConfig *config) {
  NET_GUARD;
  if (net_read_config(config) < 0)
    return socket_result(-1);
  if (!config->address) {
    errno = 100;
    return -1;
  }
  return 0;
}

static void ipv4_address(struct bionic_sockaddr *address, uint32_t ip) {
  memset(address, 0, sizeof(*address));
  address->family = 2;
  memcpy(address->data + 2, &ip, sizeof(ip));
}

int net_getifaddrs(struct bionic_ifaddrs **out) {
  if (!out) {
    errno = EINVAL;
    return -1;
  }
  *out = NULL;
  NetConfig config = {0};
  if (current_config(&config) < 0)
    return -1;
  struct Interface {
    struct bionic_ifaddrs entry;
    char name[8];
    struct bionic_sockaddr address, netmask, broadcast;
  } *interface = calloc(1, sizeof(*interface));
  if (!interface)
    return -1;
  memcpy(interface->name, "switch0", 8);
  interface->entry.name = interface->name;
  interface->entry.flags = 0x1043; /* UP | BROADCAST | RUNNING | MULTICAST */
  interface->entry.address = &interface->address;
  interface->entry.netmask = &interface->netmask;
  interface->entry.broadcast = &interface->broadcast;
  ipv4_address(&interface->address, config.address);
  ipv4_address(&interface->netmask, config.netmask);
  ipv4_address(&interface->broadcast, config.address | ~config.netmask);
  *out = &interface->entry;
  return 0;
}

void net_freeifaddrs(struct bionic_ifaddrs *list) {
  while (list) {
    struct bionic_ifaddrs *next = list->next;
    free(list);
    list = next;
  }
}

FILE *net_open_config(const char *path) {
  NetConfig config = {0};
  if (current_config(&config) < 0)
    return NULL;
  char content[256];
  int size;
  if (!strcmp(path, "/proc/net/route")) {
    size = snprintf(content, sizeof(content),
        "Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT\n"
        "switch0\t00000000\t%08X\t0003\t0\t0\t0\t00000000\t0\t0\t0\n", config.gateway);
  } else if (!strcmp(path, "/etc/resolv.conf")) {
    char dns[2][INET_ADDRSTRLEN] = {{0}};
    for (unsigned i = 0; i < 2; ++i)
      inet_ntop(AF_INET, &config.dns[i], dns[i], sizeof(dns[i]));
    size = snprintf(content, sizeof(content), "# Switch DNS\n%s%s%s%s%s%s",
        config.dns[0] ? "nameserver " : "", config.dns[0] ? dns[0] : "", config.dns[0] ? "\n" : "",
        config.dns[1] ? "nameserver " : "", config.dns[1] ? dns[1] : "", config.dns[1] ? "\n" : "");
  } else {
    errno = ENOENT;
    return NULL;
  }
  if (size < 0 || (size_t)size >= sizeof(content)) {
    errno = EOVERFLOW;
    return NULL;
  }
  FILE *file = fmemopen(NULL, (size_t)size + 1, "w+");
  if (!file)
    return NULL;
  if (fwrite(content, 1, (size_t)size, file) != (size_t)size || fseek(file, 0, SEEK_SET)) {
    fclose(file);
    return NULL;
  }
  return file;
}

int net_ioctl(int fd, unsigned long request, void *argument) {
  NET_GUARD;
  if (!argument) {
    errno = EFAULT;
    return -1;
  }
  if (request == 0x5421 || request == 0x541b)
    return socket_result(ioctl(fd, request == 0x5421 ? FIONBIO : FIONREAD, argument));
  if (request == 0x8927) {
    if (strncmp(argument, "switch0", 16)) {
      errno = 19;
      return -1;
    }
    NetConfig config = {0};
    if (current_config(&config) < 0)
      return -1;
    unsigned char *mac = (unsigned char *)argument + 16;
    memset(mac, 0, 16);
    mac[0] = 1; /* ARPHRD_ETHER */
    mac[2] = 0x02;
    memcpy(mac + 4, &config.address, 4);
    return 0;
  }
  errno = ENOTTY;
  return -1;
}

static int host_family(int family) {
  if (family == 0) return AF_UNSPEC;
  if (family == 2) return AF_INET;
  if (family == 10) return AF_INET6;
  errno = 97;
  return -1;
}

static int guest_family(int family) {
  if (family == AF_UNSPEC) return 0;
  if (family == AF_INET) return 2;
  if (family == AF_INET6) return 10;
  errno = 97;
  return -1;
}

static int host_address(const struct bionic_sockaddr *source, uint32_t length,
                        struct sockaddr_storage *target, socklen_t *target_length) {
  if (!source || length < sizeof(source->family)) {
    errno = EINVAL;
    return -1;
  }
  const int family = host_family(source->family);
  if (family < 0)
    return -1;
  const size_t size = family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
  if (length < size) {
    errno = EINVAL;
    return -1;
  }
  memset(target, 0, sizeof(*target));
  memcpy(target, source, size);
  struct sockaddr *address = (struct sockaddr *)target;
  address->sa_family = family;
#ifdef __SWITCH__
  address->sa_len = size;
#endif
  *target_length = size;
  return 0;
}

static int guest_address(const struct sockaddr *source, socklen_t source_length,
                         struct bionic_sockaddr *target, uint32_t *target_length) {
  if (!source_length) {
    *target_length = 0;
    return 0;
  }
  if (source_length < offsetof(struct sockaddr, sa_family) + sizeof(source->sa_family)) {
    errno = EINVAL;
    return -1;
  }
  const int family = guest_family(source->sa_family);
  if (family < 0)
    return -1;
  unsigned char converted[sizeof(struct sockaddr_storage)] = {0};
  if (source_length > sizeof(converted)) {
    errno = EINVAL;
    return -1;
  }
  memcpy(converted, source, source_length);
  const uint16_t encoded_family = family;
  memcpy(converted, &encoded_family, sizeof(encoded_family));
  const uint32_t copied = *target_length < source_length ? *target_length : source_length;
  if (copied)
    memcpy(target, converted, copied);
  *target_length = source_length;
  return 0;
}

static int host_flags(int flags) {
  const unsigned allowed = 0x1 | 0x2 | 0x4 | 0x20 | 0x40 | 0x80 | 0x100 | 0x4000;
  if ((unsigned)flags & ~allowed) {
    errno = 95;
    return -1;
  }
  int result = 0;
  if (flags & 0x1) result |= MSG_OOB;
  if (flags & 0x2) result |= MSG_PEEK;
  if (flags & 0x4) result |= MSG_DONTROUTE;
  if (flags & 0x20) result |= MSG_TRUNC;
  if (flags & 0x40) result |= MSG_DONTWAIT;
  if (flags & 0x80) result |= MSG_EOR;
  if (flags & 0x100) result |= MSG_WAITALL;
#ifdef MSG_NOSIGNAL
  if (flags & 0x4000) result |= MSG_NOSIGNAL;
#endif
  return result;
}

int net_socket(int family, int type, int protocol) {
  NET_GUARD;
  family = host_family(family);
  if (family < 0)
    return -1;
  if (type & ~(0xf | 0x800 | 0x80000)) {
    errno = EINVAL;
    return -1;
  }
  const int fd = socket(family, type & 0xf, protocol);
  if (fd < 0)
    return socket_result(fd);
  if (type & 0x800) {
    int enabled = 1;
    if (net_ioctl(fd, 0x5421, &enabled) < 0) {
      const int saved = errno;
      close(fd);
      errno = saved;
      return -1;
    }
  }
  return fd;
}

int net_bind(int fd, const struct bionic_sockaddr *address, uint32_t length) {
  NET_GUARD;
  struct sockaddr_storage converted;
  socklen_t size;
  if (host_address(address, length, &converted, &size) < 0)
    return -1;
  return socket_result(bind(fd, (struct sockaddr *)&converted, size));
}

int net_connect(int fd, const struct bionic_sockaddr *address, uint32_t length) {
  NET_GUARD;
  struct sockaddr_storage converted;
  socklen_t size;
  if (host_address(address, length, &converted, &size) < 0)
    return -1;
  return socket_result(connect(fd, (struct sockaddr *)&converted, size));
}

ssize_t net_sendto(int fd, const void *buffer, size_t size, int flags,
                   const struct bionic_sockaddr *address, uint32_t length) {
  NET_GUARD;
  flags = host_flags(flags);
  if (flags < 0)
    return -1;
  struct sockaddr_storage converted;
  socklen_t address_size = 0;
  if (address && host_address(address, length, &converted, &address_size) < 0)
    return -1;
  return socket_result(sendto(fd, buffer, size, flags,
      address ? (struct sockaddr *)&converted : NULL, address_size));
}

ssize_t net_recvfrom(int fd, void *buffer, size_t size, int flags,
                     struct bionic_sockaddr *address, uint32_t *length) {
  NET_GUARD;
  flags = host_flags(flags);
  if (flags < 0)
    return -1;
  if (address && !length) {
    errno = EFAULT;
    return -1;
  }
  struct sockaddr_storage converted;
  socklen_t address_size = sizeof(converted);
  const ssize_t result = socket_result(recvfrom(fd, buffer, size, flags,
      address ? (struct sockaddr *)&converted : NULL, address ? &address_size : NULL));
  if (result >= 0 && address && guest_address((struct sockaddr *)&converted,
                                             address_size, address, length) < 0)
    return -1;
  return result;
}

ssize_t net_recvmsg(int fd, struct bionic_msghdr *message, int flags) {
  NET_GUARD;
  if (!message || message->iov_count > INT_MAX) {
    errno = EINVAL;
    return -1;
  }
  flags = host_flags(flags);
  if (flags < 0)
    return -1;
  struct sockaddr_storage address;
  struct msghdr converted = {0};
  converted.msg_name = message->name ? &address : NULL;
  converted.msg_namelen = message->name ? sizeof(address) : 0;
  converted.msg_iov = message->iov;
  converted.msg_iovlen = message->iov_count;
  const ssize_t result = socket_result(recvmsg(fd, &converted, flags));
  if (result < 0)
    return result;
  if (message->name && guest_address((struct sockaddr *)&address, converted.msg_namelen,
                                     message->name, &message->name_length) < 0)
    return -1;
  message->control_length = 0;
  message->flags = (converted.msg_flags & MSG_TRUNC ? 0x20 : 0) |
                   (converted.msg_flags & MSG_CTRUNC ? 0x8 : 0) |
                   (converted.msg_flags & MSG_OOB ? 0x1 : 0) |
                   (converted.msg_flags & MSG_EOR ? 0x80 : 0);
  return result;
}

int net_select(int count, fd_set *read, fd_set *write, fd_set *error, struct timeval *timeout) {
  NET_GUARD;
  return socket_result(select(count, read, write, error, timeout));
}

static int host_option(int *level, int option) {
  static const int socket_options[][2] = {
    {1, SO_DEBUG}, {2, SO_REUSEADDR}, {3, SO_TYPE}, {4, SO_ERROR},
    {5, SO_DONTROUTE}, {6, SO_BROADCAST}, {7, SO_SNDBUF}, {8, SO_RCVBUF},
    {9, SO_KEEPALIVE}, {10, SO_OOBINLINE}, {13, SO_LINGER}, {15, SO_REUSEPORT},
    {18, SO_RCVLOWAT}, {19, SO_SNDLOWAT}, {20, SO_RCVTIMEO}, {21, SO_SNDTIMEO}
  };
  static const int ip_options[][2] = {
    {1, IP_TOS}, {2, IP_TTL}, {3, IP_HDRINCL}, {32, IP_MULTICAST_IF},
    {33, IP_MULTICAST_TTL}, {34, IP_MULTICAST_LOOP},
    {35, IP_ADD_MEMBERSHIP}, {36, IP_DROP_MEMBERSHIP}
  };
  if (*level == 1) {
    *level = SOL_SOCKET;
    for (size_t i = 0; i < sizeof(socket_options) / sizeof(socket_options[0]); ++i)
      if (socket_options[i][0] == option)
        return socket_options[i][1];
  } else if (*level == 0) {
    *level = IPPROTO_IP;
    for (size_t i = 0; i < sizeof(ip_options) / sizeof(ip_options[0]); ++i)
      if (ip_options[i][0] == option)
        return ip_options[i][1];
  } else if (*level == 6 && option == 1) {
    *level = IPPROTO_TCP;
    return TCP_NODELAY;
  }
  errno = 92;
  return -1;
}

int net_setsockopt(int fd, int level, int option, const void *value, uint32_t length) {
  NET_GUARD;
  option = host_option(&level, option);
  if (option < 0)
    return -1;
  return socket_result(setsockopt(fd, level, option, value, length));
}

int net_getsockopt(int fd, int level, int option, void *value, uint32_t *length) {
  NET_GUARD;
#ifndef __SWITCH__
  const int error_option = level == 1 && option == 4;
#endif
  option = host_option(&level, option);
  if (option < 0)
    return -1;
  const int result = socket_result(getsockopt(fd, level, option, value, length));
  // Horizon already uses Linux errno values in SO_ERROR.
#ifndef __SWITCH__
  if (result == 0 && error_option && value && length && *length >= sizeof(int)) {
    int error;
    memcpy(&error, value, sizeof(error));
    error = bionic_errno(error);
    memcpy(value, &error, sizeof(error));
  }
#endif
  return result;
}

int net_getsockname(int fd, struct bionic_sockaddr *address, uint32_t *length) {
  NET_GUARD;
  if (!address || !length) {
    errno = EFAULT;
    return -1;
  }
  struct sockaddr_storage converted;
  socklen_t size = sizeof(converted);
  const int result = socket_result(getsockname(fd, (struct sockaddr *)&converted, &size));
  if (result < 0)
    return result;
  return guest_address((struct sockaddr *)&converted, size, address, length);
}

int net_shutdown(int fd, int how) {
  NET_GUARD;
  return socket_result(shutdown(fd, how));
}

static int gai_result(int result) {
  static const int errors[][2] = {
    {EAI_AGAIN, 2}, {EAI_BADFLAGS, 3}, {EAI_FAIL, 4}, {EAI_FAMILY, 5},
    {EAI_MEMORY, 6}, {EAI_NONAME, 8}, {EAI_SERVICE, 9}, {EAI_SOCKTYPE, 10},
    {EAI_SYSTEM, 11}, {EAI_OVERFLOW, 14}
  };
  for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i)
    if (result == errors[i][0])
      return errors[i][1];
  return result ? 4 : 0;
}

void net_freeaddrinfo(struct bionic_addrinfo *list) {
  while (list) {
    struct bionic_addrinfo *next = list->next;
    free(list->canonical_name);
    free(list);
    list = next;
  }
}

int net_getaddrinfo(const char *node, const char *service,
                    const struct bionic_addrinfo *hints, struct bionic_addrinfo **result) {
  if (!result)
    return 4;
  *result = NULL;
  if (!g_net_ready)
    return 4;
  struct addrinfo converted = {0};
  if (hints) {
    converted.ai_family = host_family(hints->family);
    if (converted.ai_family < 0)
      return 5;
    converted.ai_socktype = hints->socktype;
    converted.ai_protocol = hints->protocol;
    if (hints->flags & 0x1) converted.ai_flags |= AI_PASSIVE;
    if (hints->flags & 0x2) converted.ai_flags |= AI_CANONNAME;
    if (hints->flags & 0x4) converted.ai_flags |= AI_NUMERICHOST;
    if (hints->flags & 0x8) converted.ai_flags |= AI_NUMERICSERV;
    if (hints->flags & 0x100) converted.ai_flags |= AI_ALL;
    if (hints->flags & 0x400) converted.ai_flags |= AI_ADDRCONFIG;
    if (hints->flags & (0x200 | 0x800)) converted.ai_flags |= AI_V4MAPPED;
  }
  struct addrinfo *host_list = NULL;
  const int error = getaddrinfo(node, service, hints ? &converted : NULL, &host_list);
  if (error) {
    if (error == EAI_SYSTEM)
      errno = bionic_errno(errno);
    return gai_result(error);
  }
  struct bionic_addrinfo **tail = result;
  for (const struct addrinfo *entry = host_list; entry; entry = entry->ai_next) {
    if (entry->ai_family != AF_INET && entry->ai_family != AF_INET6)
      continue;
    struct Node {
      struct bionic_addrinfo entry;
      struct sockaddr_storage address;
    } *copy = calloc(1, sizeof(*copy));
    if (!copy)
      goto allocation_failed;
    *tail = &copy->entry;
    copy->entry.flags = hints ? hints->flags : 0;
    copy->entry.family = guest_family(entry->ai_family);
    copy->entry.socktype = entry->ai_socktype;
    copy->entry.protocol = entry->ai_protocol;
    copy->entry.address = (struct bionic_sockaddr *)&copy->address;
    copy->entry.address_length = sizeof(copy->address);
    if (guest_address(entry->ai_addr, entry->ai_addrlen, copy->entry.address,
                       &copy->entry.address_length) < 0)
      goto allocation_failed;
    if (entry->ai_canonname) {
      copy->entry.canonical_name = strdup(entry->ai_canonname);
      if (!copy->entry.canonical_name)
        goto allocation_failed;
    }
    tail = &copy->entry.next;
  }
  freeaddrinfo(host_list);
  return *result ? 0 : 8;
allocation_failed:
  freeaddrinfo(host_list);
  net_freeaddrinfo(*result);
  *result = NULL;
  return 6;
}

int net_inet_pton(int family, const char *text, void *address) {
  family = host_family(family);
  if (family < 0)
    return -1;
  return socket_result(inet_pton(family, text, address));
}
