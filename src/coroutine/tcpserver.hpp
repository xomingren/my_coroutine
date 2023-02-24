#pragma once
#ifndef __CO_TCPSERVER_HPP__
#define __CO_TCPSERVER_HPP__

#include <arpa/inet.h>  //for inet_ntop

#include <map>     //for map
#include <string>  //for string

#include "coroutine.h"
namespace CO {

class TcpServer {
  using ConnectionMap = std::map<std::string, NetFd *>;

 public:
  [[maybe_unused]] static TcpServer *getInstance() {
    static TcpServer me;
    return &me;
  }

 public:
  ~TcpServer() {}

 private:
  TcpServer() = default;
  TcpServer(const TcpServer &) = delete;
  TcpServer(const TcpServer &&) = delete;
  TcpServer &operator=(const TcpServer &) = delete;
  TcpServer &operator=(const TcpServer &&) = delete;

 private:
  Callable message_handler_;
  ConnectionMap connections_;
  int next_conn_id_ = 1;
  // todo expand to ipv6
  sockaddr_in server_addr_;

 public:
  template <typename F>
  void start(F &&f) {
    message_handler_ = std::move(f);
    int listen_fd = CreateAndListenOrDie();
    CO::NetFd *listen_netfd = CO::newNetFD(listen_fd, 1, 1);
    if (nullptr == listen_netfd) {
      printf("NetFd open socket failed.\n");
      // return -1;
      exit(-1);
    }
    CO::Coroutine *p = CO::Coroutine::getInstance();
    CO::Entity *acceptor =
        p->co_create(std::bind(&TcpServer::Acceptor, TcpServer::getInstance(),
                               (void *)listen_netfd),
                     1, 0);
    if (nullptr == acceptor) {
      printf("failed to co_create listen coroutine\n");
    }
  }

 private:
  int CreateAndListenOrDie() {
    int ret = 0;
    CO::Coroutine *p = CO::Coroutine::getInstance();

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

    server_addr_.sin_family = AF_INET;
    server_addr_.sin_port = htons(CO::kPort);
    server_addr_.sin_addr.s_addr = INADDR_ANY;

    ret = bind(listen_fd, (sockaddr *)&server_addr_, sizeof(sockaddr));
    if (ret == -1) {
      ERR_EXIT("bind");
    }

    ret = listen(listen_fd, 128);
    if (ret == -1) {
      ERR_EXIT("listen");
    }
    return listen_fd;
  }
  void *Acceptor(void *arg) {
    for (;;) {
      int addrlen = static_cast<int>(sizeof server_addr_);
      sockaddr_in client_addr;
      CO::NetFd *client_netfd = co_accept(
          (CO::NetFd *)arg, reinterpret_cast<sockaddr *>(&client_addr),
          &addrlen, CO::kNerverTimeout);
      if (nullptr == client_netfd) {
        continue;
      }

      client_netfd->addr.local = server_addr_;
      client_netfd->addr.peer = client_addr;

      char name[64];
      snprintf(name, sizeof name, "%d#%d", ntohs(server_addr_.sin_port),
               next_conn_id_);
      ++next_conn_id_;
      connections_[name] = client_netfd;

      char ip_buf[INET_ADDRSTRLEN];
      bzero(ip_buf, sizeof(ip_buf));
      inet_ntop(client_addr.sin_family, &client_addr.sin_addr, ip_buf,
                sizeof(ip_buf));

      printf("new client = %s:%d, fd = %d\n", ip_buf, client_addr.sin_port,
             client_netfd->osfd);

      CO::Coroutine *p = CO::Coroutine::getInstance();
      CO::Entity *connection =
          p->co_create(std::bind(message_handler_, (void *)client_netfd), 0, 0);
      if (nullptr == connection) {
        printf("failed co_create client coroutine\n");
      }
    }
  }
};  // class tcpserver

}  // namespace CO
#endif  // __CO_TCPSERVER_HPP__
