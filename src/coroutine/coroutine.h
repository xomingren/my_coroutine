#pragma once
#ifndef __CO_COROUTINE_H__
#define __CO_COROUTINE_H__
#include <ucontext.h>  //for ucontext in impl

#include "common.hpp"
#include "returntype.h"

namespace CO {

class Time;
class Poller;
class Coroutine final {
  friend class Time;
  friend class Poller;

 public:
 private:
  Coroutine();
  Coroutine(const Coroutine &) = delete;
  Coroutine(const Coroutine &&) = delete;
  Coroutine &operator=(const Coroutine &) = delete;
  Coroutine &operator=(const Coroutine &&) = delete;

 public:
  static Coroutine *getInstance();

 public:
  template <typename F>
  Entity *co_create(F &&f, int joinable, size_t stk_size);
  void yield();
  int Poll(pollfd *pds, int npds, useconds timeout);
  Poller *get_poller() const;

  // Entity *get_sleep_queue() const { return sleep_queue_; }

 private:
  int InitOrDie(size_t coroutine_num = kMaxCoroutineSize);
  GUID IDGenerator();
  void InitQueue(DoubleLinkedList *l);

  Entity *GetFromRunableQueue(DoubleLinkedList *queue);
  PollQueue *GetFromPollerQueue(DoubleLinkedList *queue);

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

  Entity **HeapInsert(Entity *co);
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
  std::unique_ptr<Poller> poller_;
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
