#pragma once
#ifndef __CO_TEMPLATE_IMPL_HPP__
#define __CO_TEMPLATE_IMPL_HPP__

#include <string.h>  //for memset

#include <memory>  //for smart_ptr

template <typename F>
EntityPtr CO::Coroutine::co_create(F&& f, int joinable, size_t stk_size) {
  EntityPtr co(std::make_shared<Entity>());

  co->guid = IDGenerator();

  Service<decltype(f())> add_service(std::forward<F>(f));
  manager_.RegisterService<Service<decltype(f())>>(co->guid, add_service);

  std::unique_ptr<ucontext_t> pcontext(std::make_unique<ucontext_t>());
  // delete on Entity dector()
  char* stack_ptr = new char[kStacksize];
  pcontext->uc_stack.ss_sp =
      stack_ptr;  // we will use this space to do coroutines' own work
  pcontext->uc_stack.ss_size = kStacksize;
  pcontext->uc_stack.ss_flags = 0;

  co->context = std::move(pcontext);
  getcontext(co->context.get());
  // next time setcontext, turn back here and go to Wrapper() and finishi
  // coroutine life there, never comebck to below*
  makecontext(co->context.get(), Wrapper, 0);

  // *only execute those code once per coroutine

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
  AddToRunableQueueTail(co.get());
  active_count_++;
  return co;
}

#endif  //__CO_TEMPLATE_IMPL_HPP__