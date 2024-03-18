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
class TcpServer;
class Coroutine final {
  friend class Time;
  friend class Poller;
  friend class IOuring;
  friend class TcpServer;

 public:
 private:
  Coroutine();
  ~Coroutine();
  Coroutine(const Coroutine &) = delete;
  Coroutine(const Coroutine &&) = delete;
  Coroutine &operator=(const Coroutine &) = delete;
  Coroutine &operator=(const Coroutine &&) = delete;

 public:
  [[maybe_unused]] static Coroutine *getInstance();

 public:
  template <typename F>
  [[nodiscard]] EntityPtr co_create(F &&f, int joinable,
                                    [[maybe_unused]] size_t stk_size);
  void yield();
  void Wakeup();
  void OnSysQuit();

#ifdef USE_EPOLL
  [[nodiscard]] int Poll(pollfd *pds, int npds, useconds timeout);
  [[maybe_unused]] Poller *get_poller() const noexcept;
#elif defined(USE_IOURING)
  [[nodiscard]] int Poll(std::vector<UringDetailPtr> req, useconds timeout);
  [[maybe_unused]] IOuring *get_poller() const noexcept;
#endif

 private:
  [[nodiscard]] int InitOrDie(size_t coroutine_num = kMaxCoroutineSize);
  [[nodiscard]] GUID IDGenerator() noexcept;
  void InitQueue(DoubleLinkedList *l) noexcept;

  [[maybe_unused]] EntityPtr **GetFromRunableQueue(
      DoubleLinkedList *queue) const noexcept;  // fixme
  [[maybe_unused]] PollQueue *GetFromPollerQueue(
      DoubleLinkedList *queue) const noexcept;  // fixme

  void AddToIOQueue(PollQueue *node);
  void DeleteFromIOQueue(PollQueue *node);
  // some IO coroutine will stuck in io_queue, never had a change to beening
  // runnable and get themselves' context switched, they'll hold an ownership of
  // EntityPtr, so we clean io_queue on dctor. all in all, functions that called
  // SwitchContext() may never return, so we have to use shared_ptr carefully
  void ClearIOQueue();

  void AddToRunableQueueTail(Entity *e);
  void DeleteFromRunableQueue(Entity *e);
  void InsertAfterRunableQueueHead(Entity *e);

  void AddToZombieQueue();
  void DeleteFromZombieQueue();

  void InsertBefore(DoubleLinkedList *a, DoubleLinkedList *b);
  void DeleteFrom(DoubleLinkedList *node);

  void AddToSleepQueue(EntityPtr& co, useconds timeout);
  void DeleteFromSleepQueue(EntityPtr& co);
  [[maybe_unused]] EntityPtr *HeapInsert(EntityPtr& co);
  void HeapDelete(EntityPtr& co);

  void *MainLoop();
  void DoWork();
  static void Wrapper();
  void CheckSleep();
  void Schedule();
  void SwitchContext(Entity *cur, bool destroy = false);
  // the interrupt will be "delivered"
  // only when a target thread is about to block.
  void Interrupt(EntityPtr co);
  void OnExit(void *retval);
  void Cleanup(Entity *);

 private:
  static GUID id_;
  WorkerManager manager_;
  useconds last_time_checked;
  int active_count_ = 0;
  int wakeupfd_;

#ifdef USE_EPOLL
  std::unique_ptr<Poller> poller_;
#elif defined(USE_IOURING)
  std::unique_ptr<IOuring> poller_;
#endif
  EntityPtr primordial_;
  // share ownership with Tcpserver and io_queue
  EntityPtr current_coroutine_;
  EntityPtr main_coroutine_;

  DoubleLinkedList runable_queue_;
  DoubleLinkedList io_queue_;
  DoubleLinkedList zombie_queue_;
  EntityPtr sleep_queue_;
  int sleep_queue_size_;

};  // class Coroutine

#include "template_impl.hpp"
}  // namespace CO

#endif
