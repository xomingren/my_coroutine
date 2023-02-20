#pragma once
#ifndef __CO_TIME_HPP__
#define __CO_TIME_HPP__

#include <chrono>

#include "common.hpp"

namespace CO {

class Time {
 public:
  static useconds GetTime() {
    return std::chrono::system_clock::now().time_since_epoch().count();
  }

  static int st_usleep(useconds usecs) {
    Coroutine* pinstance = Coroutine::getInstance();
    Entity* e = pinstance->current_coroutine_;

    // if (me->flags & _ST_FL_INTERRUPT) {
    //   me->flags &= ~_ST_FL_INTERRUPT;
    //   errno = EINTR;
    //   return -1;
    // }

    if (usecs != kNerverTimeout) {
      e->state = State::kSleeping;
      pinstance->AddToSleepQueue(e, usecs);
    } else
      e->state = State::kSuspend;

    // e->calledbywho = 0;
    // getcontext(e->context.get());
    // if (e->calledbywho == 0) pinstance->Schedule();
    pinstance->SwitchContext(e);

    if (e->type & Interrupt) {
      e->type &= ~Interrupt;
      errno = EINTR;
      return -1;
    }

    return 0;
  }

  static int sleep_for(int seconds) {
    return st_usleep((seconds >= 0) ? seconds * (useconds)1000000LL
                                    : kNerverTimeout);
  }

};  // class CTime
}  // namespace CO
#endif  //__CO_TIME_HPP__