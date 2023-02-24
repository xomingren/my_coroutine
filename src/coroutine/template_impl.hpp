#pragma once
#ifndef __CO_TEMPLATE_IMPL_HPP__
#define __CO_TEMPLATE_IMPL_HPP__

#include <string.h>  //for memset

#include <memory>  //for xxxxx_ptr

// 创建微线程函数
template <typename F>
Entity *CO::Coroutine::co_create(F &&f, int joinable, size_t stk_size) {
  Entity *co = new Entity;
  memset(co, 0, sizeof(Entity));
  co->guid = IDGenerator();

  Service<decltype(f())> add_service(std::forward<F>(f));
  manager_.RegisterService<Service<decltype(f())>>(co->guid, add_service);

  std::unique_ptr<ucontext_t> pcontext(std::make_unique<ucontext_t>());

  char *stackptr = new char[kStacksize];
  pcontext->uc_stack.ss_sp = stackptr;      // 指定栈空间
  pcontext->uc_stack.ss_size = kStacksize;  // 指定栈空间大小
  pcontext->uc_stack.ss_flags = 0;

  co->context = std::move(pcontext);
  getcontext(co->context.get());
  makecontext(co->context.get(), Wrapper, 0);

  // 如果微线程是可joinable的，就创建一个微线程终止时使用的条件变量
  // 当该微线程终止时会等待在该条件变量上，当其他微线程调用st_thread_join函数
  // 替该微线程收尸后，该微线程才会真正终止
  // if (joinable) {
  //   co->term = st_cond_new();
  //   if (co->term == NULL) {
  //     _st_stack_free(co->stack);
  //     return NULL;
  //   }
  // }

  co->state = CO::State::kReady;
  active_count_++;
  AddToRunableQueueTail(co);

  return co;
}

#endif  //__CO_TEMPLATE_IMPL_HPP__