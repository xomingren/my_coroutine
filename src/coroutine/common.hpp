#pragma once
#ifndef __CO_COMMON_HPP__
#define __CO_COMMON_HPP__

#include <memory>  //for ptr

// why not inside of namespace? because if do, compiler see it as CO::pollfd etc
class ucontext_t;
class pollfd;
namespace CO {

// 10 kib default for now
const int kKeysMax = 16;

const int kStacksize = 10 * 1024;

const int kMaxCoroutineSize = 100 * 1024;

const int kLocalMaxIOV = 16;

enum class State {
  kRunning = 0,
  KReady = 1,
  KIOWait,
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

using GUID = int;

struct DoubleLinkedList {
  DoubleLinkedList *next;
  DoubleLinkedList *prev;
};  // struct DoubleLinkedList

// single coroutine entity
struct Entity final {
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
  int osfd;  /* Underlying OS file descriptor */
  int inuse; /* In-use flag */
  // void *private_data;         /* Per descriptor private data */
  // _st_destructor_t destructor; /* Private data destructor function */
  // void *aux_data;             /* Auxiliary data for internal use */
  struct NetFd *next; /* For putting on the free list */
};                    // struct NetFd

struct PollQueue {
  DoubleLinkedList links; /* For putting on io queue */
  Entity *coroutine;      /* Polling coroutine */
  pollfd *pds;            /* Array of poll descriptors */
  int npds;               /* Length of the array */
  int on_ioq;             /* Is it on ioq? */
};                        // struct PollQueue

// #define FD_DATA_PTR(fd) (reinterpret_cast<FDDetail
// *>(evtlist_[fd]->data.ptr)) #define FD_READ_CNT(fd)
// FD_DATA_PTR(fd)->rd_ref_cnt #define FD_WRITE_CNT(fd)
// FD_DATA_PTR(fd)->wr_ref_cnt #define FD_EXCEP_CNT(fd)
// FD_DATA_PTR(fd)->ex_ref_cnt #define FD_REVENTS(fd) FD_DATA_PTR(fd)->revents
// #define FD_RAWFD(fd) FD_DATA_PTR(fd)->rawfd

//#define FD_DATA_PTR(fd) (revtlist_[fd])
#define FD_READ_CNT(fd) revtlist_[fd]->rd_ref_cnt
#define FD_WRITE_CNT(fd) revtlist_[fd]->wr_ref_cnt
#define FD_EXCEP_CNT(fd) revtlist_[fd]->ex_ref_cnt
#define FD_REVENTS(fd) revtlist_[fd]->revents
#define FD_RAWFD(fd) revtlist_[fd]->rawfd

}  // namespace CO
#endif  //__CO_COMMON_HPP__