#pragma once
#ifndef __CO_COROUTINE_H__
#define __CO_COROUTINE_H__
#include <ucontext.h>  //for ucontext in impl

#include "common.hpp"
#include "returntype.h"

namespace CO {

class Time;
class Poller;
class IOuring;
class Coroutine final {
  friend class Time;
  friend class Poller;
  friend class IOuring;

 public:
 private:
  Coroutine();
  Coroutine(const Coroutine &) = delete;
  Coroutine(const Coroutine &&) = delete;
  Coroutine &operator=(const Coroutine &) = delete;
  Coroutine &operator=(const Coroutine &&) = delete;

 public:
  [[maybe_unused]] static Coroutine *getInstance();

 public:
  template <typename F>
  [[nodiscard]] Entity *co_create(F &&f, int joinable,
                                  [[maybe_unused]] size_t stk_size);
  void yield();

#ifdef USE_EPOLL
  [[nodiscard]] int Poll(pollfd *pds, int npds, useconds timeout);
  [[maybe_unused]] Poller *get_poller() const noexcept;
#elif defined(USE_IOURING)
  [[nodiscard]] int Poll(std::vector<UringDetail *> req, useconds timeout);
  [[maybe_unused]] IOuring *get_poller() const noexcept;
#endif

 private:
  [[nodiscard]] int InitOrDie(size_t coroutine_num = kMaxCoroutineSize);
  [[nodiscard]] GUID IDGenerator() noexcept;
  void InitQueue(DoubleLinkedList *l) noexcept;

  [[maybe_unused]] Entity *GetFromRunableQueue(
      DoubleLinkedList *queue) const noexcept;  // fixme
  [[maybe_unused]] PollQueue *GetFromPollerQueue(
      DoubleLinkedList *queue) const noexcept;  // fixme

  void AddToIOQueue(PollQueue *node);
  void DeleteFromIOQueue(PollQueue *node);

  void AddToRunableQueueTail(Entity *e);
  void DeleteFromRunableQueue(Entity *e);
  void InsertAfterRunableQueueHead(Entity *e);

  void AddToSleepQueue(Entity *co, useconds timeout);
  void DeleteFromSleepQueue(Entity *e);

  void AddToZombieQueue();
  void DeleteFromZombieQueue();

  void InsertBefore(DoubleLinkedList *a, DoubleLinkedList *b);
  void DeleteFrom(DoubleLinkedList *node);

  [[maybe_unused]] Entity **HeapInsert(Entity *co);
  void HeapDelete(Entity *co);

  void *MainLoop();
  void DoWork();
  static void Wrapper();
  void CheckSleep();
  void Schedule();
  void SwitchContext(Entity *cur);
  void OnExit(void *retval);
  void Cleanup(Entity *);

 private:
  static GUID id_;
  WorkerManager manager_;

#ifdef USE_EPOLL
  std::unique_ptr<Poller> poller_;
#elif defined(USE_IOURING)
  std::unique_ptr<IOuring> poller_;
#endif

  Entity *current_coroutine_;
  Entity *main_coroutine_;

  useconds last_time_checked;

  DoubleLinkedList runable_queue_; /* run queue for this vp */
  DoubleLinkedList io_queue_;      /* io queue for this vp */
  DoubleLinkedList zombie_queue_;  /* zombie queue for this vp */

  Entity *sleep_queue_;  /* sleep queue for this vp */
  int sleep_queue_size_; /* number of threads on sleep queue */

  int active_count_ = 0;
};  // class Coroutine

#include "template_impl.hpp"
}  // namespace CO

#endif
