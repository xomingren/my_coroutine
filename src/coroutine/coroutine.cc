#include "coroutine.h"

#include <assert.h>  //for assert
#include <poll.h>    //for pollfd
#include <stddef.h>  //for __builtin_offsetof

#ifdef USE_EPOLL
#include "poller.h"
#elif defined(USE_IOURING)
#include "iouring.hpp"
#endif

#include "time.hpp"

using namespace CO;
using namespace std;

GUID CO::Coroutine::id_ = 0;

GUID CO::Coroutine::IDGenerator() noexcept { return id_++; }

void CO::Coroutine::InitQueue(DoubleLinkedList *l) noexcept {
  l->next = l;
  l->prev = l;
}

void CO::Coroutine::DeleteFromRunableQueue(Entity *e) {
  // e->links.prev->next = e->links.next;
  // e->links.next->prev = e->links.prev;
  DeleteFrom(&e->links);
}

CO::Coroutine::Coroutine() {
  memset(&runable_queue_, 0, sizeof(runable_queue_));
  main_coroutine_ = nullptr;
  last_time_checked = 0;
  memset(&zombie_queue_, 0, sizeof(zombie_queue_));
  sleep_queue_ = nullptr;
  sleep_queue_size_ = 0;
#ifdef USE_EPOLL
  poller_ = make_unique<Poller>();
#elif defined(USE_IOURING)
  poller_ = make_unique<IOuring>();
#endif

  assert(-1 != InitOrDie());
}

Coroutine *CO::Coroutine::getInstance() {
  static Coroutine me;
  return &me;
}

void CO::Coroutine::yield() {
  Entity *co = current_coroutine_;

  CheckSleep();
  // no need to yield
  if (runable_queue_.next == &runable_queue_) {
    return;
  }

  co->state = CO::State::kReady;
  AddToRunableQueueTail(co);
  SwitchContext(co);
}

int CO::Coroutine::InitOrDie(size_t coroutine_num) {
  if (active_count_) {
    /* Already initialized */
    return 0;
  }

  // init runable队列、io等待队列、zombie队列
  InitQueue(&runable_queue_);
  InitQueue(&io_queue_);
  InitQueue(&zombie_queue_);

  // 调用IO多路复用函数对应的初始化函数
  [[unlikely]] if (poller_->pollerInitOrDie() < 0)
    return -1;
  last_time_checked = Time::GetTime();

  // 创建idle微线程，idle微线程主要用于调用epoll等待IO事件和处理定时器
  main_coroutine_ =
      co_create(bind(&CO::Coroutine::MainLoop, this), 0, kStacksize);
  [[unlikely]] if (nullptr == main_coroutine_)
    return -1;
  main_coroutine_->type = CO::Type::Main;
  active_count_--;
  // 将idle微线程从runable队列移除，由于idle微线程只在没有微线程可以运行时，才会主动调度，
  // 所以不需要加入到run队列
  DeleteFromRunableQueue(main_coroutine_);

  // 初始化primordial微线程，primordial微线程用来标记系统进程，由于可以直接使用
  // 系统进程的栈空间，故只需要为primordial微线程分配st_thread_t和私有key数据区
  Entity *primordial = new Entity;
  memset(primordial, 0, sizeof(Entity));
  primordial->guid = IDGenerator();
  // primordial =  (Entity *)calloc(1, sizeof(Entity) + (kKeysMax * sizeof(void
  // *)));
  [[unlikely]] if (nullptr == primordial)
    return -1;
  // co->private_data = (void **)(co + 1);
  primordial->state = CO::State::kRunning;
  primordial->type = CO::Type::Primordial;

  std::unique_ptr<ucontext_t> pcontext(std::make_unique<ucontext_t>());

  char *stackptr = new char[kStacksize];
  pcontext.get()->uc_stack.ss_sp = stackptr;      // 指定栈空间
  pcontext.get()->uc_stack.ss_size = kStacksize;  // 指定栈空间大小
  pcontext.get()->uc_stack.ss_flags = 0;
  // context.uc_link = &here;//设置后继上下文
  primordial->context = std::move(pcontext);

  current_coroutine_ = primordial;
  active_count_++;

  // 当前运行的微线程是primordial微线程，当primordial微线程退出时，整个进程也会终止
  return 0;
}

Entity *CO::Coroutine::GetFromRunableQueue(
    DoubleLinkedList *queue) const noexcept {
  return ((Entity *)((char *)(queue) - __builtin_offsetof(Entity, links)));
}

PollQueue *CO::Coroutine::GetFromPollerQueue(
    DoubleLinkedList *queue) const noexcept {
  return (
      (PollQueue *)((char *)(queue) - __builtin_offsetof(PollQueue, links)));
}

void CO::Coroutine::AddToIOQueue(PollQueue *node) {
  InsertBefore(&node->links, &io_queue_);
}

void CO::Coroutine::DeleteFromIOQueue(PollQueue *node) {
  DeleteFrom(&node->links);
}

void CO::Coroutine::AddToRunableQueueTail(Entity *e) {
  InsertBefore(&e->links, &runable_queue_);
}

void CO::Coroutine::InsertBefore(DoubleLinkedList *a, DoubleLinkedList *b) {
  a->next = b;
  a->prev = b->prev;
  b->prev->next = a;
  b->prev = a;
}

void CO::Coroutine::DeleteFrom(DoubleLinkedList *node) {
  node->prev->next = node->next;
  node->next->prev = node->prev;
}

void *CO::Coroutine::MainLoop() {
  Entity *main = current_coroutine_;
  while (active_count_ > 0) {
    // main coro handle event(like epollwait or time expired) here
    poller_->Dispatch();

    /* Check sleep queue for expired threads */
    CheckSleep();

    // 交出运行权，并从runable队列st_vp_t.run_q中调度下一个微线程运行
    main->state = CO::State::kReady;
    SwitchContext(main);
  }

  /* No more threads */
  exit(0);

  /* NOTREACHED */
  return nullptr;
}

// 微线程调度逻辑，即调度下一个runable状态微线程运行
void CO::Coroutine::Schedule() {
  Entity *co;
  current_coroutine_->calledbywho = 1;
  // 如果runable队列_st_this_vp.run_q非空，就选队列首的微线程
  // 否则调度idle微线程运行
  if (runable_queue_.next != &runable_queue_) {
    co = GetFromRunableQueue(runable_queue_.next);
    DeleteFromRunableQueue(co);
  } else {
    co = main_coroutine_;
  }

  assert(co->state == CO::State::kReady);
  co->state = CO::State::kRunning;

  // 如果函数 setcontext 执行成功，那么调用 setcontext
  // 的函数将不会返回，因为当前 CPU
  // 的上下文已经交给其他函数或者过程了，当前函数完全放弃了 对 CPU 的所有权。
  current_coroutine_ = co;
  setcontext(co->context.get());
}

void CO::Coroutine::SwitchContext(Entity *cur) {
  cur->calledbywho = 0;
  getcontext(cur->context.get());
  if (cur->calledbywho == 0) Coroutine::getInstance()->Schedule();
}

void CO::Coroutine::DoWork() {
  Entity *co = current_coroutine_;

  co->retval = manager_.Excute<void *>(co->guid);  // fixme

  OnExit(co->retval);
}

void CO::Coroutine::Wrapper() { Coroutine::getInstance()->DoWork(); }

void CO::Coroutine::OnExit(void *returnval) {
  Entity *co = current_coroutine_;

  co->retval = returnval;

  // 释放微线程运行期间调用st_thread_setspecific设置的私有key数据
  Cleanup(co);

  active_count_--;

  // 如果创建了term条件变量，需要通知调用st_thread_join()等待该微线程的微线程为该
  // 微线程“收尸”
  // if (co->term) {
  // 	// 添加到zombie队列
  // 	co->state = _ST_ST_ZOMBIE;
  // 	_ST_ADD_ZOMBIEQ(co);

  // 	// 通知等待在term条件变量上的微线程
  // 	st_cond_signal(co->term);

  // 	// 交出控制权，等到为本线程收尸的微线程调用st_thread_join()返回
  // 	// 后，本微线程才会switch回来，并恢复运行
  // 	_ST_SWITCH_CONTEXT(co);

  // 	// 清理条件变量
  // 	st_cond_destroy(co->term);
  // 	co->term = NULL;
  // }

  // 如果终止的不是Primordial微线程，就释放为微线程分配的私有栈空间，
  // 释放的栈空间会放到空闲链表中
  // if (!(co->flags & _ST_FL_PRIMORDIAL))
  //	_st_stack_free(co->stack);

  // 交出控制权，并调度下一个runable状态的微线程，微线程生命周期终止
  SwitchContext(co);
  // co->calledbywho = 0;
  // getcontext(co->context.get());
  // if (co->calledbywho == 0) Schedule();

  /* Not going to land here */
}

void CO::Coroutine::Cleanup(Entity *co) {
  // int key;

  // for (key = 0; key < key_max; key++) {
  //   if (co->private_data[key] && _st_destructors[key]) {
  //     (*_st_destructors[key])(co->private_data[key]);
  //     co->private_data[key] = NULL;
  //   }
  // }
}

void CO::Coroutine::CheckSleep(void) {
  Entity *coroutine;
  useconds now;

  now = Time::GetTime();

  last_time_checked = now;

  while (sleep_queue_ != nullptr) {
    coroutine = sleep_queue_;

    assert(coroutine->type & Type::OnSleepQueue);

    if (coroutine->due > now) break;
    DeleteFromSleepQueue(coroutine);

    /* If co is waiting on condition variable, set the time out flag */
    // if (co->state == _ST_ST_COND_WAIT) co->flags |=
    // _ST_FL_TIMEDOUT;

    /* Make co runnable */
    assert(!(coroutine->type & Type::Main));
    coroutine->state = CO::State::kReady;
    // Insert at the head of RunQ, to execute timer first.
    InsertAfterRunableQueueHead(coroutine);
  }
}

void CO::Coroutine::InsertAfterRunableQueueHead(Entity *e) {
  e->links.next = runable_queue_.next;
  e->links.prev = &runable_queue_;
  runable_queue_.next->prev = &e->links;
  runable_queue_.next = &e->links;
}

void CO::Coroutine::AddToSleepQueue(Entity *co, useconds timeout) {
  co->due = last_time_checked + timeout;
  co->type |= OnSleepQueue;
  co->heap_index = ++sleep_queue_size_;
  HeapInsert(co);
}

/*
 * Insert "co" into the timeout heap, in the position
 * specified by co->heap_index.  See docs/timeout_heap.txt
 * for details about the timeout heap.
 */
Entity **CO::Coroutine::HeapInsert(Entity *co) {
  int target = co->heap_index;
  int s = target;
  Entity **p = &sleep_queue_;
  int bits = 0;
  int bit;
  int index = 1;

  while (s) {
    s >>= 1;
    bits++;
  }
  for (bit = bits - 2; bit >= 0; bit--) {
    if (co->due < (*p)->due) {
      Entity *t = *p;
      co->pleft = t->pleft;
      co->pright = t->pright;
      *p = co;
      co->heap_index = index;
      co = t;
    }
    index <<= 1;
    if (target & (1 << bit)) {
      p = &((*p)->pright);
      index |= 1;
    } else {
      p = &((*p)->pleft);
    }
  }
  co->heap_index = index;
  *p = co;
  co->pleft = co->pright = nullptr;
  return p;
}

void CO::Coroutine::HeapDelete(Entity *co) {
  Entity *t, **p;
  int bits = 0;
  int s, bit;

  /* First find and unlink the last heap element */
  p = &sleep_queue_;
  s = sleep_queue_size_;
  while (s) {
    s >>= 1;
    bits++;
  }
  for (bit = bits - 2; bit >= 0; bit--) {
    if (sleep_queue_size_ & (1 << bit)) {
      p = &((*p)->pright);
    } else {
      p = &((*p)->pleft);
    }
  }
  t = *p;
  *p = nullptr;
  --sleep_queue_size_;
  if (t != co) {
    /*
     * Insert the unlinked last element in place of the element we are
     * deleting
     */
    t->heap_index = co->heap_index;
    p = HeapInsert(t);
    t = *p;
    t->pleft = co->pleft;
    t->pright = co->pright;

    /*
     * Reestablish the heap invariant.
     */
    for (;;) {
      Entity *y; /* The younger child */
      int index_tmp;
      if (t->pleft == nullptr)
        break;
      else if (t->pright == nullptr)
        y = t->pleft;
      else if (t->pleft->due < t->pright->due)
        y = t->pleft;
      else
        y = t->pright;
      if (t->due > y->due) {
        Entity *tl = y->pleft;
        Entity *tr = y->pright;
        *p = y;
        if (y == t->pleft) {
          y->pleft = t;
          y->pright = t->pright;
          p = &y->pleft;
        } else {
          y->pleft = t->pleft;
          y->pright = t;
          p = &y->pright;
        }
        t->pleft = tl;
        t->pright = tr;
        index_tmp = t->heap_index;
        t->heap_index = y->heap_index;
        y->heap_index = index_tmp;
      } else {
        break;
      }
    }
  }
  co->pleft = co->pright = nullptr;
}

#ifdef USE_EPOLL

// pollfd is for compatible with poll and kqueue, may consider change it
int CO::Coroutine::Poll(pollfd *pds, int npds, useconds timeout) {
  pollfd *pd;
  pollfd *epd = pds + npds;
  PollQueue pq;
  Entity *me = current_coroutine_;
  int n;

  // if (me->flags & _ST_FL_INTERRUPT) {
  //   me->flags &= ~_ST_FL_INTERRUPT;
  //   errno = EINTR;
  //   return -1;
  // }

  if (poller_->RegisterEvent(pds, npds) < 0) return -1;

  pq.pds = pds;
  pq.npds = npds;
  pq.coroutine = me;
  pq.on_ioq = 1;
  AddToIOQueue(&pq);
  if (timeout != kNerverTimeout) AddToSleepQueue(me, timeout);
  me->state = State::kIOWait;

  SwitchContext(me);

  n = 0;
  if (pq.on_ioq) {
    // if we timedout, the pollq might still be on the ioq. remove it
    DeleteFromIOQueue(&pq);
    poller_->RemoveEvent(pds, npds);
  } else {
    // count the number of ready descriptors
    for (pd = pds; pd < epd; pd++) {
      if (pd->revents) n++;
    }
  }

  // if (me->flags & _ST_FL_INTERRUPT) {
  //   me->flags &= ~_ST_FL_INTERRUPT;
  //   errno = EINTR;
  //   return -1;
  // }

  return n;
}

Poller *CO::Coroutine::get_poller() const noexcept { return poller_.get(); }

#elif defined(USE_IOURING)

// pollfd is for compatible with poll and kqueue, may consider change it
int CO::Coroutine::Poll(vector<UringDetail *> data, useconds timeout) {
  PollQueue node;
  Entity *me = current_coroutine_;
  int n;

  // if (me->flags & _ST_FL_INTERRUPT) {
  //   me->flags &= ~_ST_FL_INTERRUPT;
  //   errno = EINTR;
  //   return -1;
  // }

  if (poller_->submit() <= 0) return -1;
  node.fdlist = data;
  node.coroutine = me;
  node.on_ioq = 1;
  AddToIOQueue(&node);
  if (timeout != kNerverTimeout) AddToSleepQueue(me, timeout);
  me->state = State::kIOWait;

  SwitchContext(me);

  n = 0;
  if (node.on_ioq) {
    /* If we timed out, the pollq might still be on the ioq. Remove it */
    DeleteFromIOQueue(&node);
    // poller_->seen();
  } else {
    /* Count the number of ready descriptors */
    for (auto i : node.fdlist) {
      if (i->is_active) ++n;
    }
  }

  // if (me->flags & _ST_FL_INTERRUPT) {
  //   me->flags &= ~_ST_FL_INTERRUPT;
  //   errno = EINTR;
  //   return -1;
  // }

  return n;
}

IOuring *CO::Coroutine::get_poller() const noexcept { return poller_.get(); }

#endif

void CO::Coroutine::DeleteFromSleepQueue(Entity *coroutine) {
  HeapDelete(coroutine);
  coroutine->type &= ~OnSleepQueue;
}
