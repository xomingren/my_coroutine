#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <iostream>

#include "../coroutine/co_io.hpp"
#include "../coroutine/coroutine.h"
#include "../coroutine/time.hpp"

const int kPort = 8088;

#define ERR_EXIT(m) \
  do {              \
    perror(m);      \
    exit(-1);       \
  } while (0)
using namespace CO;
using namespace std;

Coroutine *co;

void *TcpConnection(void *arg) {
  NetFd *client_netfd = (NetFd *)arg;
  int client_fd = client_netfd->osfd;

  sockaddr_in client_addr;
  socklen_t client_addr_len = sizeof(client_addr);
  int ret = getpeername(client_fd, (sockaddr *)&client_addr, &client_addr_len);
  if (ret == -1) {
    printf("failed to get client ip: %s\n", strerror(ret));
  }

  char ip_buf[INET_ADDRSTRLEN];
  bzero(ip_buf, sizeof(ip_buf));
  inet_ntop(client_addr.sin_family, &client_addr.sin_addr, ip_buf,
            sizeof(ip_buf));

  while (1) {
    char buf[1024] = {0};
    // here the first time fd get from accept going into AddFD and
    // become
    ssize_t ret = co_read(client_netfd, buf, sizeof(buf), kNerverTimeout);
    if (ret == -1) {
      printf("client co_read error\n");
      break;
    } else if (ret == 0) {
      printf("client quit, fd = %d, ip = %s\n", client_netfd->osfd, ip_buf);
      closeNetFD(client_netfd);
      break;
    }

    printf("recv from %s, data = %s\n", ip_buf, buf);

    ret = co_write(client_netfd, buf, ret, kNerverTimeout);
    if (ret == -1) {
      printf("client co_write error\n");
    }
  }
  return nullptr;
}

void *Acceptor(void *arg) {
  while (1) {
    NetFd *client_netfd = co_accept((NetFd *)arg, NULL, NULL, kNerverTimeout);
    if (client_netfd == NULL) {
      continue;
    }

    printf("get a new client, fd = %d\n", (client_netfd->osfd));

    Entity *connection =
        co->co_create(bind(TcpConnection, (void *)client_netfd), 0, 0);
    if (connection == NULL) {
      printf("failed co_create client coroutine\n");
    }
  }
}

int main() {
  int ret = 0;
  co = Coroutine::getInstance();

  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd == -1) {
    ERR_EXIT("socket");
  }

  int reuse_socket = 1;
  ret = setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_socket,
                   sizeof(int));
  if (ret == -1) {
    ERR_EXIT("setsockopt");
  }

  sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(kPort);
  server_addr.sin_addr.s_addr = INADDR_ANY;

  ret = bind(listen_fd, (sockaddr *)&server_addr, sizeof(sockaddr));
  if (ret == -1) {
    ERR_EXIT("bind");
  }

  ret = listen(listen_fd, 128);
  if (ret == -1) {
    ERR_EXIT("listen");
  }

  NetFd *co_netfd = CO::newNetFD(listen_fd, 1, 1);
  if (!co_netfd) {
    printf("NetFd open socket failed.\n");
    return -1;
  }

  Entity *acceptor = co->co_create(std::bind(Acceptor, (void *)co_netfd), 1, 0);
  if (acceptor == nullptr) {
    printf("failed to co_create listen coroutine\n");
  }

  while (1) {
    CO::Time::sleep_for(1);
  }

  return 0;
}
