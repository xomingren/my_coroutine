#include "coroutine.h"

#include <assert.h>       //for assert
#include <poll.h>         //for pollfd
#include <stddef.h>       //for __builtin_offsetof
#include <sys/eventfd.h>  //for eventfd

#ifdef USE_EPOLL
#include "poller.h"
#elif defined(USE_IOURING)
#include "iouring.hpp"
#endif

#include "tcpserver.hpp"
#include "time.hpp"

using namespace CO;
using namespace std;

GUID CO::Coroutine::id_ = 0;

GUID CO::Coroutine::IDGenerator() noexcept { return id_++; }

void CO::Coroutine::InitQueue(DoubleLinkedList *l) noexcept {
  l->next = l;
  l->prev = l;
}

void CO::Coroutine::DeleteFromRunableQueue(Entity *e) { DeleteFrom(&e->links); }

CO::Coroutine::Coroutine() {
  // std::cout << "in Coroutine" << std::endl;
  main_coroutine_ = nullptr;
  last_time_checked = 0;
  sleep_queue_ = nullptr;
  sleep_queue_size_ = 0;
#ifdef USE_EPOLL
  poller_ = make_unique<Poller>();
#elif defined(USE_IOURING)
  poller_ = make_unique<IOuring>();
#endif

  if (InitOrDie() < 0) exit(-1);
}

CO::Coroutine::~Coroutine() {
  // ClearIOQueue();
}

Coroutine *CO::Coroutine::getInstance() {
  static Coroutine me;
  return &me;
}

void CO::Coroutine::yield() {
  Entity *co = current_coroutine_.get();

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
    // already initialized, never happen
    return 0;
  }

  InitQueue(&runable_queue_);
  InitQueue(&io_queue_);
  InitQueue(&zombie_queue_);

  [[unlikely]] if (poller_->pollerInitOrDie() != 0)
    return -1;
  wakeupfd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  poller_->setWakeup(wakeupfd_);

  last_time_checked = Time::GetTime();

  // init main, eventloop and handle time event here
  main_coroutine_ =
      co_create(bind(&CO::Coroutine::MainLoop, this), 0, kStacksize);
  [[unlikely]] if (nullptr == main_coroutine_)
    return -1;
  main_coroutine_->smart_ptr_addr = reinterpret_cast<void *>(&main_coroutine_);
  main_coroutine_->type = CO::Type::Main;
  active_count_--;
  // main_coroutine_ only runs when no other running
  DeleteFromRunableQueue(main_coroutine_.get());

  primordial_ = make_shared<Entity>();
  primordial_->smart_ptr_addr = reinterpret_cast<void *>(&primordial_);

  primordial_->guid = IDGenerator();
  [[unlikely]] if (nullptr == primordial_)
    return -1;
  primordial_->state = CO::State::kRunning;
  primordial_->type = CO::Type::Primordial;

  std::unique_ptr<ucontext_t> pcontext(
      std::make_unique<ucontext_t>());  // don't need to allocate memory for
                                        // ucontext_t.uc_stack.ss_sp
  // since we won't do makecontext on primordial thread, system will init
  // minimize it so we can still use get/set
  primordial_->context = std::move(pcontext);

  current_coroutine_ = primordial_;
  active_count_++;

  return 0;
}
// raw data emplace in the start of shared_ptr
EntityPtr **CO::Coroutine::GetFromRunableQueue(
    DoubleLinkedList *queue) const noexcept {
  // return ((Entity *)((char *)(queue) - __builtin_offsetof(Entity, links)));
  return reinterpret_cast<shared_ptr<Entity> **>(
      ((char *)(queue) + sizeof(DoubleLinkedList)));
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

void CO::Coroutine::ClearIOQueue() {
  for (DoubleLinkedList *node = io_queue_.next; node != &io_queue_;
       node = node->next) {
    auto pq = GetFromPollerQueue(node);

    for (auto &i : pq->fdlist) {
      i.reset();
      // std::cout << "fdlist:::::" << i.use_count() << std::endl;
    }
    std::vector<UringDetailPtr> empty;
    pq->fdlist.swap(empty);
    DeleteFromIOQueue(pq);
    pq->coroutine.reset();
  }
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
  Entity *main = current_coroutine_.get();
  while (active_count_ > 0) {
    poller_->Dispatch();

    // check sleep queue for expired threads
    CheckSleep();

    main->state = CO::State::kReady;
    SwitchContext(main);
  }

  // no more threads
  exit(0);

  // never reach here
  return nullptr;
}

// Schedule: find the next runnable coroutine from queue
void CO::Coroutine::Schedule() {
  EntityPtr me;
  current_coroutine_->return_from_scheduler = true;
  if (runable_queue_.next != &runable_queue_) {
    me = **(GetFromRunableQueue(runable_queue_.next));
    DeleteFromRunableQueue(((me.get())));
  } else {
    me = main_coroutine_;
  }

  assert(me->state == CO::State::kReady);
  me->state = CO::State::kRunning;

  current_coroutine_ = ((me));
  Entity *raw = me.get();
  me.reset();  // this function never return, so we have to reset here
  setcontext(((raw))->context.get());

  // never reach here
}

void CO::Coroutine::SwitchContext(Entity *cur, bool destroy) {
  [[likely]] if (!destroy) {
    cur->return_from_scheduler = false;
    getcontext(cur->context.get());
    if (false == cur->return_from_scheduler)
      Coroutine::getInstance()->Schedule();
  } else {  // goto quit-clean branch
    TcpServer *server = TcpServer::getInstance();
    EntityPtr me = **(GetFromRunableQueue(&cur->links));
    server->closeConnection(me);
    me.reset();  // this function never return, so we have to reset here
    Coroutine::getInstance()->Schedule();
  }
}

void CO::Coroutine::Interrupt(EntityPtr co) {
  // if co is already dead
  if (co->state == State::kZombie) return;

  co->type |= Type::Interrupt;

  if (co->state == State::kRunning || co->state == State::kReady) return;

  if (co->type & Type::OnSleepQueue) DeleteFromSleepQueue(co);

  co->state = State::kReady;
  AddToRunableQueueTail(co.get());
}

void CO::Coroutine::DoWork() {
  Entity *co = current_coroutine_.get();

  co->retval = manager_.Excute<void *>(co->guid);  // fixme

  OnExit(co->retval);
}

void CO::Coroutine::Wrapper() { Coroutine::getInstance()->DoWork(); }

void CO::Coroutine::OnExit(void *returnval) {
  Entity *co = current_coroutine_.get();

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
  SwitchContext(co, true);

  // not going to land here
}

void CO::Coroutine::Cleanup(Entity *co) {}

void CO::Coroutine::CheckSleep(void) {
  EntityPtr coroutine;
  useconds now;

  now = Time::GetTime();

  last_time_checked = now;

  while (sleep_queue_ != nullptr) {
    coroutine = sleep_queue_;

    assert(coroutine->type & Type::OnSleepQueue);

    if (coroutine->due > now) break;
    DeleteFromSleepQueue(coroutine);

    // if co is waiting on condition variable, set the time out flag
    //  if (co->state == _ST_ST_COND_WAIT) co->flags |=
    //  _ST_FL_TIMEDOUT;

    // Make co runnable
    assert(!(coroutine->type & Type::Main));
    coroutine->state = CO::State::kReady;
    // Insert at the head of RunQ, to execute timer first.
    InsertAfterRunableQueueHead(coroutine.get());
  }
}

void CO::Coroutine::InsertAfterRunableQueueHead(Entity *e) {
  e->links.next = runable_queue_.next;
  e->links.prev = &runable_queue_;
  runable_queue_.next->prev = &e->links;
  runable_queue_.next = &e->links;
}

void CO::Coroutine::AddToSleepQueue(EntityPtr &co, useconds timeout) {
  co->due = last_time_checked + timeout;
  co->type |= OnSleepQueue;
  co->heap_index = ++sleep_queue_size_;  // heap_index start with 1
  // cout << co.use_count() << endl;
  HeapInsert(co);
  // cout << co.use_count() << endl;
}

EntityPtr *CO::Coroutine::HeapInsert(EntityPtr &co) {
  int target = co->heap_index;
  int s = target;
  auto *p = &(sleep_queue_);
  int bits = 0;
  int bit;
  int index = 1;

  while (s) {
    s >>= 1;
    bits++;
  }
  // heap_index start with 1, and the leaf level should be excluded, so minus 2
  for (bit = bits - 2; bit >= 0; bit--) {
    if (co->due < (*p)->due) {
      EntityPtr t = *p;
      co->pleft = t->pleft;
      co->pright = t->pright;
      *p = co;
      co->heap_index = index;
      co = t;
    }
    index <<= 1;  // left child = 2 * N
    // for instance: heap_index = 10(1010),
    // highest bit is always 1,ignored,so we get a binary sequence 010,
    // from top to bottom, follow the rules, 1-right, 0-left
    // goes like this(left->right->left),compare the due in each position
    if (target & (1 << bit)) {
      p = &((*p)->pright);
      index |= 1;  // right child = 2 * N + 1
    } else {
      p = &((*p)->pleft);  // left child = 2 * N
    }
  }
  co->heap_index = index;
  *p = co;
  co->pleft = co->pright = nullptr;

  // cout << co.use_count() << endl;
  // cout << (*p).use_count() << endl;
  return p;
}

void CO::Coroutine::DeleteFromSleepQueue(EntityPtr &coroutine) {
  HeapDelete(coroutine);
  coroutine->type &= ~OnSleepQueue;
}

void CO::Coroutine::HeapDelete(EntityPtr &co) {
  EntityPtr t, *p;
  int bits = 0;
  int s, bit;

  // first find and unlink the last heap element
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
    // Insert the unlinked last element in place of the element we are deleting

    t->heap_index = co->heap_index;
    p = HeapInsert(t);
    t = *p;
    t->pleft = co->pleft;
    t->pright = co->pright;

    // reestablish the heap invariant.

    for (;;) {
      EntityPtr y;  // the younger child
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
        EntityPtr tl = y->pleft;
        EntityPtr tr = y->pright;
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

  if (me->type & Interrupt) {
    me->type &= ~Interrupt;
    errno = EINTR;
    return -1;
  }

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

  if (me->type & Interrupt) {
    me->type &= ~Interrupt;
    errno = EINTR;
    return -1;
  }

  return n;
}

Poller *CO::Coroutine::get_poller() const noexcept { return poller_.get(); }

#elif defined(USE_IOURING)

int CO::Coroutine::Poll(vector<UringDetailPtr> data, useconds timeout) {
  PollQueue node;
  int n;

  if (current_coroutine_->type & Type::Interrupt) {
    current_coroutine_->type &= ~Type::Interrupt;
    errno = EINTR;
    return -1;
  }

  if (poller_->submit() <= 0) return -1;
  node.fdlist = data;
  node.coroutine = current_coroutine_;
  node.on_ioq = 1;
  AddToIOQueue(&node);
  if (timeout != kNerverTimeout) AddToSleepQueue(current_coroutine_, timeout);
  current_coroutine_->state = State::kIOWait;

  SwitchContext(current_coroutine_.get());

  n = 0;
  if (1 == node.on_ioq) {
    // if we timed out, the pollq might still be on the ioq. remove it
    DeleteFromIOQueue(&node);
  } else if (0 == node.on_ioq) {
    // count the number of ready descriptors
    for (auto i : node.fdlist) {  // fixme wind up don't need this
      if (i->is_active) ++n;
    }
  } else {
    return -2;
  }

  if (current_coroutine_->type & Type::Interrupt) {
    current_coroutine_->type &= ~Type::Interrupt;
    errno = EINTR;
    return -1;
  }

  return n;
}

IOuring *CO::Coroutine::get_poller() const noexcept { return poller_.get(); }

#endif

void Coroutine::Wakeup() {
  uint64_t one = 1;
  ssize_t n = ::write(wakeupfd_, &one, sizeof one);
  [[unlikely]] if (n != sizeof one) {}
  OnSysQuit();
}

void CO::Coroutine::OnSysQuit() {
  // append current_coroutine_ to the very tail, make sure this wind up
  // coroutine(precisely it was primordial_) execute behind all other
  // coroutines
  main_coroutine_->state = State::kReady;
  current_coroutine_->state = State::kReady;
  AddToRunableQueueTail(main_coroutine_.get());
  AddToRunableQueueTail(current_coroutine_.get());

  // execute main, so we can goto dispatch() and execute all the blocking
  // coroutine there, set them to runnable
  SwitchContext(current_coroutine_.get());
}
