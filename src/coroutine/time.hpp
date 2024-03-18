#pragma once
#ifndef __CO_TIME_HPP__
#define __CO_TIME_HPP__

#include <chrono>  //for std::chrono

#include "common.hpp"

namespace CO {

class Time {
 public:
  [[maybe_unused]] static useconds GetTime() {
    return std::chrono::system_clock::now().time_since_epoch().count();
  }

  static int st_usleep(useconds usecs) {
    Coroutine* pinstance = Coroutine::getInstance();
    EntityPtr e = pinstance->current_coroutine_;

    if (e->type & Type::Interrupt) {
      e->type &= ~Type::Interrupt;
      errno = EINTR;
      return -1;
    }

    if (usecs != kNerverTimeout) {
      e->state = State::kSleeping;
      pinstance->AddToSleepQueue(e, usecs);
    } else
      e->state = State::kSuspend;

    pinstance->SwitchContext(e.get());

    if (e->type & Type::Interrupt) {
      e->type &= ~Type::Interrupt;
      errno = EINTR;
      return -1;
    }

    return 0;
  }

  static int sleep_for(int seconds) {  // fixme 1000s = 1000 times
    return st_usleep((seconds >= 0) ? seconds * static_cast<useconds>(1000000LL)
                                    : kNerverTimeout);
  }

};  // class CTime
}  // namespace CO
#endif  //__CO_TIME_HPP__