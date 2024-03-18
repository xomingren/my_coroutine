#pragma once
#ifndef __CO_COMMON_HPP__
#define __CO_COMMON_HPP__

#include <netinet/in.h>  //for sockaddr_in
#include <ucontext.h>    //for ucontext

#include <functional>  //for std::function
#include <iostream>    //for debug
#include <memory>      //for ptr
#include <vector>      //for vector

//* * * master switch of poller * * *
// #define USE_EPOLL
#define USE_IOURING
//* * * master switch of poller * * *

// why not inside of namespace? because if do, compiler see it as CO::pollfd etc
class pollfd;
class io_uring_cqe;
namespace CO {

#ifdef USE_IOURING
class UringDetail;
#endif

const int kPort = 8088;

const int kMaxFD = 1000;

const int kStacksize = 4096;

const int kMaxCoroutineSize = 100 * 1024;

const int kLocalMaxIOV = 16;

enum class State {
  kRunning = 0,
  kReady = 1,
  kIOWait,
  kSuspend,
  kZombie,
  kSleeping
};

enum Type {
  Primordial = 0x01,
  Main = 0x02,
  OnSleepQueue = 0x04,
  Interrupt = 0x08,
  Timeout = 0x10
};

using useconds = unsigned long long;
const useconds kNerverTimeout = (static_cast<useconds>(-1LL));  // ULLONG_MAX

// fixme : expand to modern c++ template task
using Callable = std::function<void *(void *)>;

using GUID = int;

struct DoubleLinkedList {
  DoubleLinkedList *next = nullptr;
  DoubleLinkedList *prev = nullptr;
};  // struct DoubleLinkedList

// single coroutine entity
struct [[nodiscard]] Entity final {
  ~Entity() {
    delete[] reinterpret_cast<char *>(context.get()->uc_stack.ss_sp);
    context.get()->uc_stack.ss_sp = nullptr;
    std::cout << "out Entity range" << std::endl;
  }
  // put the links to front of the struct so we on't have to calculate the
  // offset
  DoubleLinkedList links;  // for putting on run/io/zombie queue
  void *smart_ptr_addr = nullptr;

  GUID guid = -1;
  std::unique_ptr<ucontext_t> context;

  void *retval = nullptr;

  State state = State::kZombie;
  int type = 0;

  bool return_from_scheduler = false;

  // wakeup time when co is sleeping
  useconds due = 0;
  // for organized on sleep queue
  std::shared_ptr<Entity> pleft;
  std::shared_ptr<Entity> pright;
  int heap_index;
};  // struct Entity
using EntityPtr = std::shared_ptr<Entity>;

using UringDetailPtr = std::shared_ptr<UringDetail>;
struct NetFD {
  ~NetFD() { std::cout << "out NetFD range" << std::endl; }
  int inuse = 0;  // in-use flag
  int osfd = -1;  // underlying OS file descriptor
#ifdef USE_EPOLL
// void *private_data = nullptr; // per descriptor private data
#elif defined(USE_IOURING)
  UringDetailPtr private_data;
#endif
  //  NetFD *next = nullptr; // fixme using slub, putting on the free list, to
  //  improve performance
};  // struct NetFD
using NetFDPtr = std::shared_ptr<NetFD>;

struct PollQueue {
  ~PollQueue() {  // std::cout << "out pollqueue" << std::endl;
  }
  DoubleLinkedList links;
  std::shared_ptr<Entity> coroutine;

#ifdef USE_EPOLL
  pollfd *pds;
  int npds;
#elif defined(USE_IOURING)
  std::vector<UringDetailPtr> fdlist;
#endif
  // 0: not in, 1: in, -1: quit program
  int on_ioq;
};  // struct PollQueue

#define FD_READ_CNT(fd) revtlist_[fd]->rd_ref_cnt
#define FD_WRITE_CNT(fd) revtlist_[fd]->wr_ref_cnt
#define FD_EXCEP_CNT(fd) revtlist_[fd]->ex_ref_cnt
#define FD_REVENTS(fd) revtlist_[fd]->revents
#define FD_RAWFD(fd) revtlist_[fd]->rawfd

#define ERR_EXIT(m) \
  do {              \
    perror(m);      \
    exit(-1);       \
  } while (0)

}  // namespace CO
#endif  //__CO_COMMON_HPP__