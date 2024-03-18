#include "poller.h"

#ifdef USE_EPOLL

#include <errno.h>      //for errno
#include <fcntl.h>      //for fcntl()
#include <poll.h>       //for pollfd
#include <sys/epoll.h>  //for epoll

#include "coroutine.h"

using namespace CO;
using namespace std;

CO::Poller::Poller()
    : maxfd_(kMaxFD),
      epfd_(-1),
      evtlist_(maxfd_, epoll_event()),
      revtlist_(maxfd_, nullptr) {
  for (int i = 0; i < revtlist_.size(); ++i) {
    revtlist_[i] = new FDDetail;  // fixme
    memset(revtlist_[i], 0, sizeof(FDDetail));
    revtlist_[i]->rawfd = i;
  }
}

CO::Poller::~Poller() {
  vector<epoll_event> empty;
  evtlist_.swap(empty);
  for (int i = 0; i < revtlist_.size(); ++i) {
    delete revtlist_[i];
    revtlist_[i] = nullptr;
  }
  vector<FDDetail *> emptyr;
  revtlist_.swap(emptyr);
}

int CO::Poller::GetEvents(int fd) const noexcept {
  int events = 0;
  events |= (FD_READ_CNT(fd) ? EPOLLIN : 0);
  events |= (FD_WRITE_CNT(fd) ? EPOLLOUT : 0);
  events |= (FD_EXCEP_CNT(fd) ? EPOLLPRI : 0);
  return events;
}

int CO::Poller::pollerInitOrDie(void) {
  int err = 0;

  [[unlikely]] if ((epfd_ = epoll_create(1)) < 0) {
    err = errno;
    return -1;
  }
  fcntl(epfd_, F_SETFD, O_NONBLOCK);

  return 0;
}

int CO::Poller::CheckValidFD(int osfd) const noexcept {
  [[unlikely]] if (osfd >= maxfd_)
    return -1;

  return 0;
}

void CO::Poller::RemoveEvent(pollfd *pds, int npds) {
  epoll_event ev;
  pollfd *pd;
  pollfd *epd = pds + npds;
  int old_events, events, op;

  for (pd = pds; pd < epd; ++pd) {
    old_events = GetEvents(pd->fd);
    // decrease the event count that fullfilled
    if (pd->events & POLLIN) FD_READ_CNT(pd->fd)
    --;
    if (pd->events & POLLOUT) FD_WRITE_CNT(pd->fd)
    --;
    if (pd->events & POLLPRI) FD_EXCEP_CNT(pd->fd)
    --;

    events = GetEvents(pd->fd);
    /*
     * The FD_REVENTS check below is needed so we can use
     * this function inside dispatch(). Outside of dispatch()
     * FD_REVENTS is always zero for all descriptors.
     */
    if (events != old_events && FD_REVENTS(pd->fd) == 0) {
      op = events ? EPOLL_CTL_MOD /*still had work to do*/ : EPOLL_CTL_DEL;
      ev.events = events;
      // ev.data.fd = pd->fd;
      ev.data.ptr = reinterpret_cast<void *>(revtlist_[pd->fd]);
      if (epoll_ctl(epfd_, op, pd->fd, &ev) == 0 && op == EPOLL_CTL_DEL) {
        // epolldata_->evtlist_cnt--;
      }
    }
  }
}

int CO::Poller::RegisterEvent(pollfd *pds, int npds) {
  epoll_event ev;
  int i, fd;
  int old_events, events, op;

  for (i = 0; i < npds; i++) {
    fd = pds[i].fd;
    [[unlikely]] if (fd < 0 || !pds[i].events ||
                     (pds[i].events & ~(POLLIN | POLLOUT | POLLPRI))) {
      errno = EINVAL;
      return -1;
    }
    [[unlikely]] if (fd >= maxfd_)  // fixme
      return -1;
  }

  for (i = 0; i < npds; i++) {
    fd = pds[i].fd;
    old_events = GetEvents(fd);

    if (pds[i].events & POLLIN) FD_READ_CNT(fd)++;
    if (pds[i].events & POLLOUT) FD_WRITE_CNT(fd)++;
    if (pds[i].events & POLLPRI) FD_EXCEP_CNT(fd)++;

    events = GetEvents(fd);
    // check the new fd is complete new or existed
    // if complete new,use add,else use mod
    if (events != old_events) {
      op = old_events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;

      ev.events = events;
      events |= EPOLLET;
      FD_RAWFD(fd) = fd;
      ev.data.ptr = reinterpret_cast<void *>(revtlist_[fd]);

      // evtlist_[fd]->events = events;

      if (epoll_ctl(epfd_, op, fd, &ev) < 0 &&
          (op != EPOLL_CTL_ADD || errno != EEXIST))
        break;
    }
  }

  [[unlikely]] if (i < npds) {
    /* Error */
    int err = errno;
    /* Unroll the state */
    RemoveEvent(pds, i + 1);
    errno = err;
    return -1;
  }

  return 0;
}

int CO::Poller::PrepareCloseFD(int osfd) const noexcept {
  if (FD_READ_CNT(osfd) || FD_WRITE_CNT(osfd) || FD_EXCEP_CNT(osfd)) {
    errno = EBUSY;
    return -1;
  }
  return 0;
}

void CO::Poller::Dispatch(void) {
  useconds min_timeout;
  DoubleLinkedList *q;
  PollQueue *pq;
  pollfd *pds, *epds;
  epoll_event ev;
  int timeout, nfd, i, osfd, activated;
  int events, op;
  short revents;

  Coroutine *pinstance = Coroutine::getInstance();
  // sleep_queue is empty means main coroutine is
  if (nullptr == pinstance->sleep_queue_) {
    timeout = -1;
  } else {
    min_timeout =
        (pinstance->sleep_queue_->due <= pinstance->last_time_checked)
            ? 0
            : (pinstance->sleep_queue_->due - pinstance->last_time_checked);
    timeout = (int)(min_timeout / 1000);

    // At least wait 1ms when <1ms, to avoid epoll_wait spin loop.
    if (timeout == 0) {
      timeout = 1;
    }
  }

  /* Check for I/O operations */
  nfd = epoll_wait(epfd_, evtlist_.data(), maxfd_, timeout);

  if (nfd > 0) {  // if not timeout
    // fill revents with its fd
    for (i = 0; i < nfd; i++) {
      osfd = reinterpret_cast<FDDetail *>(evtlist_[i].data.ptr)->rawfd;
      FD_REVENTS(osfd) = evtlist_[i].events;
      if (FD_REVENTS(osfd) & (EPOLLERR | EPOLLHUP)) {
        // also set I/O bits on error
        //  error happend, save the events before this loop till next loop
        FD_REVENTS(osfd) |= GetEvents(osfd);
      }
    }
    // check revents
    // one coroutine from ioq may contain several fds,check it all
    for (q = pinstance->io_queue_.next; q != &pinstance->io_queue_;
         q = q->next) {
      pq = pinstance->GetFromPollerQueue(q);
      activated = 0;
      // how many fd on single node of ioq
      epds = pq->pds + pq->npds;

      for (pds = pq->pds; pds < epds; pds++) {
        if (FD_REVENTS(pds->fd) == 0) {
          pds->revents = 0;
          continue;
        }
        osfd = pds->fd;
        events = pds->events;
        revents = 0;  // comfirm twice, this is the real revent
        if ((events & POLLIN) && (FD_REVENTS(osfd) & EPOLLIN))
          revents |= POLLIN;
        if ((events & POLLOUT) && (FD_REVENTS(osfd) & EPOLLOUT))
          revents |= POLLOUT;
        if ((events & POLLPRI) && (FD_REVENTS(osfd) & EPOLLPRI))
          revents |= POLLPRI;
        if (FD_REVENTS(osfd) & EPOLLERR) revents |= POLLERR;
        if (FD_REVENTS(osfd) & EPOLLHUP) revents |= POLLHUP;

        pds->revents = revents;
        if (revents) {
          activated = 1;
        }
      }  // 1 coroutine checked

      if (activated) {
        pinstance->DeleteFromIOQueue(pq);
        pq->on_ioq = 0;
        /*
         * Here we will only delete/modify descriptors that
         * didn't fire (see comments in RemoveEvent()).
         */
        RemoveEvent(pq->pds, pq->npds);

        if (pq->coroutine->type & CO::Type::OnSleepQueue)
          pinstance->DeleteFromSleepQueue(pq->coroutine);
        pq->coroutine->state = CO::State::kReady;
        pinstance->AddToRunableQueueTail(pq->coroutine);
      }
    }  // all ioq checked,move ready coroutine from sleepq(epoll return from
       // timeout) and ioq to runnableq

    for (i = 0; i < nfd; i++) {
      // Delete/modify descriptors that fired
      // nowthat all happend events recorded on PollQueue->pollfd
      // restore reventslist to default
      osfd = reinterpret_cast<FDDetail *>(evtlist_[i].data.ptr)->rawfd;
      FD_REVENTS(osfd) = 0;
      // after deleteevent, still had events remain? add to epfd again
      events = GetEvents(osfd);
      op = events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
      ev.events = events;
      ev.data.ptr = reinterpret_cast<void *>(revtlist_[i]);
      if (epoll_ctl(epfd_, op, osfd, &ev) == 0 && op == EPOLL_CTL_DEL) {
        // epolldata_->evtlist_cnt--;
      }
    }
  }  // if not time out
}
#endif  // use epoll
