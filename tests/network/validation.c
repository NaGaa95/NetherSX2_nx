#include "../../source/network.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static unsigned checks;
static int config_available = 1;
static NetConfig configuration;

static void check(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "%s (errno %d)\n", message, errno);
    exit(EXIT_FAILURE);
  }
  ++checks;
}

int net_read_config(NetConfig *config) {
  if (!config_available) {
    errno = ENETDOWN;
    return -1;
  }
  *config = configuration;
  return 0;
}

static void wait_ready(int fd, int writing) {
  fd_set descriptors;
  FD_ZERO(&descriptors);
  FD_SET(fd, &descriptors);
  struct timeval timeout = {2, 0};
  check(net_select(fd + 1, writing ? NULL : &descriptors,
        writing ? &descriptors : NULL, NULL, &timeout) == 1, "socket readiness");
}

static struct bionic_sockaddr loopback(void) {
  struct bionic_sockaddr address = {0};
  address.family = 2;
  check(net_inet_pton(2, "127.0.0.1", address.data + 2) == 1, "IPv4 conversion");
  return address;
}

static void discovery(void) {
  struct bionic_ifaddrs *list = (void *)1;
  check(net_getifaddrs(&list) == -1 && !list && errno == 100, "disabled adapter discovery");
  check(net_getifaddrs(NULL) == -1 && errno == EINVAL, "null discovery output");
  errno = EBUSY;
  net_freeifaddrs(NULL);
  check(errno == EBUSY, "free null adapter");
  g_net_ready = 1;
  for (unsigned i = 0; i < 16; ++i) {
    check(net_getifaddrs(&list) == 0 && list && !list->next, "active adapter discovery");
    check(!strcmp(list->name, "switch0") && (list->flags & 1) && !(list->flags & 8),
          "adapter is up and not loopback");
    check(list->address->family == 2 && list->netmask->family == 2 &&
          !memcmp(list->address->data + 2, &configuration.address, 4) &&
          !memcmp(list->netmask->data + 2, &configuration.netmask, 4), "Android IPv4 interface ABI");
    struct in_addr broadcast;
    inet_pton(AF_INET, "192.168.1.255", &broadcast);
    check(!memcmp(list->broadcast->data + 2, &broadcast, 4), "interface broadcast address");
    net_freeifaddrs(list);
  }
  config_available = 0;
  list = (void *)1;
  check(net_getifaddrs(&list) == -1 && !list && errno == 100, "disconnected adapter discovery");
  config_available = 1;
  uint32_t saved = configuration.address;
  configuration.address = 0;
  check(net_getifaddrs(&list) == -1 && !list && errno == 100, "missing IP address");
  configuration.address = saved;

  char request[40];
  memset(request, 0x55, sizeof(request));
  memcpy(request, "switch0", 8);
  check(net_ioctl(1, 0x8927, request) == 0 && request[16] == 1 && request[17] == 0 &&
        request[18] == 2 && !memcmp(request + 20, &configuration.address, 4) &&
        request[32] == 0x55 && request[39] == 0x55, "Android MAC ioctl ABI");
  memset(request, 0, sizeof(request));
  check(net_ioctl(1, 0x8927, request) == -1 && errno == 19, "unknown adapter MAC request");
}

static void metadata(void) {
  char content[512] = {0};
  FILE *file = net_open_config("/proc/net/route");
  check(file != NULL, "open gateway metadata");
  size_t size = fread(content, 1, sizeof(content) - 1, file);
  check(size > 0 && !ferror(file) && fclose(file) == 0, "read gateway metadata");
  char name[16];
  unsigned destination, gateway;
  char *line = strchr(content, '\n');
  check(line && sscanf(line + 1, "%15s %x %x", name, &destination, &gateway) == 3 &&
        !strcmp(name, "switch0") && destination == 0 && gateway == configuration.gateway,
        "automatic adapter selection can find its default gateway");
  file = net_open_config("/etc/resolv.conf");
  check(file != NULL, "open DNS metadata");
  memset(content, 0, sizeof(content));
  size = fread(content, 1, sizeof(content) - 1, file);
  check(size > 0 && fclose(file) == 0 && content[0] == '#' &&
        strstr(content, "nameserver 9.9.9.9\n") && strstr(content, "nameserver 1.1.1.1\n"),
        "core DNS parser can read both configured servers");
  check(net_open_config("/unknown") == NULL && errno == ENOENT, "unknown metadata path");
}

static void udp(void) {
  int server = net_socket(2, 2, 17);
  int client = net_socket(2, 2 | 0x800 | 0x80000, 17);
  check(server >= 0 && client >= 0, "create UDP sockets with Android flags");
  check(fcntl(client, F_GETFL) & O_NONBLOCK, "nonblocking socket creation");
  int enabled = 1;
  check(net_setsockopt(server, 1, 2, &enabled, sizeof(enabled)) == 0, "SO_REUSEADDR conversion");
  check(net_setsockopt(client, 1, 6, &enabled, sizeof(enabled)) == 0, "SO_BROADCAST conversion");
  struct bionic_sockaddr address = loopback();
  check(net_bind(server, &address, sizeof(address)) == 0, "bind Android UDP address");
  uint32_t length = sizeof(address);
  check(net_getsockname(server, &address, &length) == 0 && length == 16 && address.family == 2,
        "return Android bound address");
  check(net_sendto(client, "hello", 5, 0, &address, length) == 5, "send UDP payload");
  wait_ready(server, 0);
  int available = 0;
  check(net_ioctl(server, 0x541b, &available) == 0 && available == 5, "Android FIONREAD conversion");
  char buffer[8] = {0};
  struct bionic_sockaddr from;
  length = sizeof(from);
  check(net_recvfrom(server, buffer, sizeof(buffer), 0, &from, &length) == 5 &&
        !memcmp(buffer, "hello", 5) && from.family == 2 && length == 16, "UDP response and peer address");
  check(net_recvfrom(client, buffer, sizeof(buffer), 0x40, NULL, NULL) == -1 && errno == 11,
        "Android MSG_DONTWAIT and EAGAIN");
  check(net_sendto(client, "hello", 5, 0, &address, sizeof(address)) == 5, "send recvmsg payload");
  wait_ready(server, 0);
  struct iovec pieces[] = {{buffer, 2}, {buffer + 2, 3}};
  struct bionic_msghdr message = {0};
  message.name = &from;
  message.name_length = sizeof(from);
  message.iov = pieces;
  message.iov_count = 2;
  memset(buffer, 0, sizeof(buffer));
  check(net_recvmsg(server, &message, 0) == 5 && !memcmp(buffer, "hello", 5) &&
        message.name_length == 16 && from.family == 2 && message.control_length == 0,
        "Android recvmsg scatter/gather and address ABI");
  unsigned char truncated[3] = {0xcc, 0xcc, 0xcc};
  length = 1;
  check(net_getsockname(server, (void *)truncated, &length) == 0 && length == 16 &&
        truncated[0] == 2 && truncated[1] == 0xcc && truncated[2] == 0xcc, "bounded socket address copy");
  check(net_setsockopt(client, 1, 9999, &enabled, sizeof(enabled)) == -1 && errno == 92,
        "unknown options must not reach an unrelated native option");
  close(server);
  close(client);
}

static void tcp(void) {
  int server = socket(AF_INET, SOCK_STREAM, 0);
  check(server >= 0, "create TCP listener");
  struct sockaddr_in endpoint = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  check(bind(server, (void *)&endpoint, sizeof(endpoint)) == 0 && listen(server, 1) == 0,
        "listen on loopback");
  socklen_t native_length = sizeof(endpoint);
  check(getsockname(server, (void *)&endpoint, &native_length) == 0, "get listener port");
  struct bionic_sockaddr address = loopback();
  memcpy(address.data, &endpoint.sin_port, sizeof(endpoint.sin_port));
  int client = net_socket(2, 1, 6);
  int nonblocking = 1;
  check(client >= 0 && net_ioctl(client, 0x5421, &nonblocking) == 0, "Android FIONBIO conversion");
  check(net_setsockopt(client, 6, 1, &nonblocking, sizeof(nonblocking)) == 0, "TCP_NODELAY conversion");
  int result = net_connect(client, &address, sizeof(address));
  check(result == 0 || (result == -1 && errno == 115), "nonblocking connect error conversion");
  wait_ready(client, 1);
  int error = -1;
  uint32_t length = sizeof(error);
  check(net_getsockopt(client, 1, 4, &error, &length) == 0 && error == 0, "SO_ERROR conversion");
  int peer = accept(server, NULL, NULL);
  check(peer >= 0, "accept bridged client");
  check(net_sendto(client, "ping", 4, 0x4000, NULL, 0) == 4, "connected TCP send and MSG_NOSIGNAL");
  char buffer[4];
  check(recv(peer, buffer, sizeof(buffer), MSG_WAITALL) == 4 && !memcmp(buffer, "ping", 4),
        "TCP request reached native peer");
  check(send(peer, "pong", 4, 0) == 4, "TCP reply");
  wait_ready(client, 0);
  check(net_recvfrom(client, buffer, sizeof(buffer), 0, NULL, NULL) == 4 && !memcmp(buffer, "pong", 4),
        "TCP reply reached bridged client");
  check(send(peer, "data", 4, 0) == 4, "TCP recvmsg reply");
  wait_ready(client, 0);
  struct iovec piece = {buffer, sizeof(buffer)};
  struct bionic_msghdr message = {0};
  message.name = &address;
  message.name_length = sizeof(address);
  message.iov = &piece;
  message.iov_count = 1;
  check(net_recvmsg(client, &message, 0) == 4 && !memcmp(buffer, "data", 4) &&
        message.name_length == 0, "TCP recvmsg without a peer address");
  check(net_shutdown(client, 2) == 0, "TCP shutdown");
  close(peer);
  close(client);
  close(server);
}

static void resolver(void) {
  struct bionic_addrinfo hints = {0}, *list = NULL;
  hints.family = 2;
  hints.socktype = 2;
  hints.flags = 0xc;
  check(net_getaddrinfo("127.0.0.1", "1234", &hints, &list) == 0 && list, "numeric IPv4 resolution");
  for (const struct bionic_addrinfo *entry = list; entry; entry = entry->next) {
    uint16_t port;
    memcpy(&port, entry->address->data, sizeof(port));
    check(entry->family == 2 && entry->address_length == 16 && entry->address->family == 2 &&
          ntohs(port) == 1234, "Android addrinfo layout and address");
  }
  net_freeaddrinfo(list);
  hints.family = 10;
  check(net_getaddrinfo("::1", "1234", &hints, &list) == 0 && list && list->family == 10 &&
        list->address->family == 10 && list->address_length == 28, "IPv6 family conversion");
  net_freeaddrinfo(list);
  hints.family = 2;
  check(net_getaddrinfo("not-a-numeric-address", "1234", &hints, &list) == 8 && !list,
        "Android resolver error conversion");
  net_freeaddrinfo(NULL);
}

int main(void) {
  inet_pton(AF_INET, "192.168.1.50", &configuration.address);
  inet_pton(AF_INET, "255.255.255.0", &configuration.netmask);
  inet_pton(AF_INET, "192.168.1.1", &configuration.gateway);
  inet_pton(AF_INET, "9.9.9.9", &configuration.dns[0]);
  inet_pton(AF_INET, "1.1.1.1", &configuration.dns[1]);
  discovery();
  metadata();
  udp();
  tcp();
  resolver();
  g_net_ready = 0;
  check(net_socket(2, 2, 17) == -1 && errno == 100, "disabled socket guard");
  printf("%u network checks passed\n", checks);
  return 0;
}
