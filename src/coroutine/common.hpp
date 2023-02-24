#pragma once
#ifndef __CO_COMMON_HPP__
#define __CO_COMMON_HPP__

#include <netinet/in.h>  //for sockaddr_in

#include <functional>  //for std::function
#include <memory>      //for ptr
#include <vector>      //for vector

// #define USE_EPOLL
#define USE_IOURING

// why not inside of namespace? because if do, compiler see it as CO::pollfd etc
class ucontext_t;
class pollfd;
class io_uring_cqe;
namespace CO {

#ifdef USE_IOURING
class UringDetail;
#endif
const int kPort = 8088;

// 10 kib default for now
const int kKeysMax = 16;

const int kStacksize = 10 * 1024;

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
const useconds kNerverTimeout = ((useconds)-1LL);

// fixme : expand to modern c++ template task
using Callable = std::function<void *(void *)>;

using GUID = int;

struct DoubleLinkedList {
  DoubleLinkedList *next;
  DoubleLinkedList *prev;
};  // struct DoubleLinkedList

// single coroutine entity
struct [[nodiscard]] Entity final {
  // 第一个非静态成员变量的地址是相同
  // For putting on run/sleep/zombie queue
  DoubleLinkedList links;

  GUID guid = -1;
  std::unique_ptr<ucontext_t> context;

  void *retval;

  State state;
  int type;
  int calledbywho;
  // Wakeup time when co is sleeping
  useconds due;
  Entity *pleft;
  Entity *pright;
  int heap_index;
};  // struct Entity

struct NetFd {
  struct NetAddress {
    sockaddr_in local;
    sockaddr_in peer;
  };
  int inuse; /* In-use flag */
  int osfd;  /* Underlying OS file descriptor */
  NetAddress addr;

  void *private_data; /* Per descriptor private data */
  // _st_destructor_t destructor; /* Private data destructor function */
  // void *aux_data;             /* Auxiliary data for ssinternal use */
  struct NetFd *next; /* For putting on the free list */
};                    // struct NetFd

struct PollQueue {
  DoubleLinkedList links; /* For putting on io queue */
  Entity *coroutine;      /* Polling coroutine */

#ifdef USE_EPOLL
  pollfd *pds; /* Array of poll descriptors */
  int npds;    /* Length of the array */
#elif defined(USE_IOURING)
  std::vector<UringDetail *> fdlist;
#endif

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