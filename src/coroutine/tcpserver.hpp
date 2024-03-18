#pragma once
#ifndef __CO_TCPSERVER_HPP__
#define __CO_TCPSERVER_HPP__

#include <arpa/inet.h>  //for inet_ntop

#include <map>     //for map
#include <string>  //for string

#include "co_io.hpp"
#include "time.hpp"
namespace CO {

class TcpServer {
  friend class Coroutine;
#ifdef USE_EPOLL
  using ConnectionMap = std::map<std::string, std::shared_ptr<NetFD>>;
#elif defined(USE_IOURING)
  using ConnectionMap = std::map<EntityPtr, NetFDPtr>;
#endif

 public:
  [[maybe_unused]] static TcpServer *getInstance() {
    static TcpServer me;
    return &me;
  }

 public:
  ~TcpServer() {
    // all client supposed to be closed gentlely through this
    CO::Coroutine::getInstance()->Wakeup();
    // always empty
    assert(connections_.empty());
    // if (!connections_.empty()) {
    //   ConnectionMap empty;
    //   connections_.swap(empty);
    // }
    // std::cout << "out class TcpServer" << std::endl;
  }

 private:
  TcpServer() {
    CO::Coroutine::getInstance();  // ensure sequence
    // std::cout << "in tcpserver" << std::endl;
  };
  TcpServer(const TcpServer &) = delete;
  TcpServer(const TcpServer &&) = delete;
  TcpServer &operator=(const TcpServer &) = delete;
  TcpServer &operator=(const TcpServer &&) = delete;

 private:
  NetFDPtr listen_fd_;
  EntityPtr acceptor_;
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
    listen_fd_ = CO::NewNetFD(listen_fd, 1, 1, 0);
    if (nullptr == listen_fd_) {
      printf("NetFD open socket failed.\n");
      exit(-1);
    }
    CO::Coroutine *p = CO::Coroutine::getInstance();

    acceptor_ = p->co_create(
        std::bind(&TcpServer::Acceptor, TcpServer::getInstance()), 1, 0);
    if (nullptr == acceptor_) {
      printf("failed to co_create listen coroutine\n");
    }
    acceptor_->smart_ptr_addr = reinterpret_cast<void *>(&acceptor_);
  }
  void closeConnection(EntityPtr me) { connections_.erase(me); }

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
  void *Acceptor() {
    for (;;) {
      uint addrlen = sizeof server_addr_;
      sockaddr_in client_addr;
      NetFDPtr client_netfd;
      auto opt = co_accept(listen_fd_.get(),
                           reinterpret_cast<sockaddr *>(&client_addr), &addrlen,
                           CO::kNerverTimeout);
      if (opt.has_value()) {
        [[unlikely]] if (nullptr == opt.value()) {
          // if (errno == EINTR)  // user called in coroutine::poll()
          //   continue;
          continue;
        }
        client_netfd = opt.value();
      } else
        break;

      char ip_buf[INET_ADDRSTRLEN];
      bzero(ip_buf, sizeof(ip_buf));
      inet_ntop(client_addr.sin_family, &client_addr.sin_addr, ip_buf,
                sizeof(ip_buf));

      printf("new client = %s:%d, fd = %d\n", ip_buf, client_addr.sin_port,
             client_netfd->osfd);

      CO::Coroutine *p = CO::Coroutine::getInstance();
      auto connection = p->co_create(
          (std::bind(message_handler_,
                     reinterpret_cast<void *>(client_netfd.get()))),
          0, 0);
      if (nullptr == connection) {
        printf("failed co_create client coroutine\n");
      }

      connections_.insert(std::make_pair(connection, client_netfd));
      auto iter = connections_.find(connection);
      auto tmp = const_cast<EntityPtr *>(&(iter->first));
      connection->smart_ptr_addr = reinterpret_cast<void *>(tmp);
    }
    return nullptr;
  }
};  // class tcpserver

}  // namespace CO
#endif  // __CO_TCPSERVER_HPP__
