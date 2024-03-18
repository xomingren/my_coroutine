// #include "../coroutine/co_io.hpp"
// #include "../coroutine/coroutine.h"
#include "../coroutine/tcpserver.hpp"
#include "../coroutine/time.hpp"

using namespace CO;
using namespace std;

void *Echo(void *arg) {
  auto client_netfd = reinterpret_cast<NetFD *>(arg);
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

  for (;;) {
    char buf[200] = {0};
    ssize_t ret = co_read(client_netfd, buf, sizeof(buf), kNerverTimeout);
    if (-1 == ret) {
      printf("client co_read error\n");
      if (closeNetFD(client_netfd) < 0) printf("close fd error\n");
      break;  // may interrupt by user, try again fixme
    } else if (0 == ret) {
      printf("client quit, fd = %d, ip = %s:%d\n", client_netfd->osfd, ip_buf,
             client_addr.sin_port);
      if (closeNetFD(client_netfd) < 0) printf("close fd error\n");
      break;
    } else if (-2 == ret) {
      if (closeNetFD(client_netfd, false) < 0) printf("close fd error\n");
      break;
    }

    printf("recv from %s:%d, data = %s\n", ip_buf, client_addr.sin_port, buf);

    ret = co_write(client_netfd, buf, ret, kNerverTimeout);
    if (ret == -1) {
      printf("client co_write error\n");
    }
  }
  return nullptr;
}
int timeout = 0;
void *Foo(void *arg) {
  timeout += 1;
  int tmp = timeout;
  for (;;) {
    printf("timeout: %d\n", tmp);
    CO::Time::sleep_for(tmp);
  }
  return nullptr;
}
int main() {
  TcpServer *server = TcpServer::getInstance();
  server->start(bind(Echo, placeholders::_1));

  EntityPtr tasks[30];
  for (int co_nums = 0; co_nums <= 29; ++co_nums) {
    tasks[co_nums] =
        Coroutine::getInstance()->co_create(bind(Foo, nullptr), 1, 1);
    tasks[co_nums]->smart_ptr_addr = &tasks[co_nums];  // fixme
  }

  for (;;) {
    CO::Time::sleep_for(1);
  }
  int i = 110;
  for (; i > 0; --i) {
    CO::Time::sleep_for(1);
  }

  return 0;
}
