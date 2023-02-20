#include "poller.h"

#include <errno.h>
#include <fcntl.h>  //for fcntl()
#include <poll.h>   //for pollfd
#include <sys/epoll.h>

#include "coroutine.h"

using namespace CO;
using namespace std;

CO::Poller::Poller()
    : maxfd_(10000),
      epfd_(-1),
      evtlist_(maxfd_, epoll_event()),
      revtlist_(maxfd_, nullptr) {
  // for (int i = 0; i < evtlist_.size(); ++i) {
  //   evtlist_[i] = new epoll_event;
  //   memset(evtlist_[i], 0, sizeof(epoll_event));
  //   // evtlist_[i]->data.ptr = new FDDetail;
  //   // FD_RAWFD(i) = i;
  // }
  for (int i = 0; i < revtlist_.size(); ++i) {
    revtlist_[i] = new FDDetail;
    memset(revtlist_[i], 0, sizeof(FDDetail));
    revtlist_[i]->rawfd = i;
  }
}

CO::Poller::~Poller() {
  // for (int i = 0; i < evtlist_.size(); ++i) {
  //   // delete reinterpret_cast<FDDetail *>(evtlist_[i]->data.ptr);
  //   // evtlist_[i]->data.ptr = nullptr;
  //   delete evtlist_[i];
  //   evtlist_[i] = nullptr;
  // }
  vector<epoll_event> empty;
  evtlist_.swap(empty);
  for (int i = 0; i < revtlist_.size(); ++i) {
    // delete reinterpret_cast<FDDetail *>(evtlist_[i]->data.ptr);
    // evtlist_[i]->data.ptr = nullptr;
    delete revtlist_[i];
    revtlist_[i] = nullptr;
  }
  vector<FDDetail *> emptyr;
  revtlist_.swap(emptyr);
}

int CO::Poller::GetEvents(int fd) const {
  int events = 0;
  events |= (FD_READ_CNT(fd) ? EPOLLIN : 0);
  events |= (FD_WRITE_CNT(fd) ? EPOLLOUT : 0);
  events |= (FD_EXCEP_CNT(fd) ? EPOLLPRI : 0);
  return events;
}

int CO::Poller::pollerInitOrDie(void) {
  int err = 0;

  if ((epfd_ = epoll_create(1)) < 0) {
    err = errno;
    return -1;
  }
  fcntl(epfd_, F_SETFD, O_NONBLOCK);

  return 0;
}

int CO::Poller::CheckValidFD(int osfd) const {
  if (osfd >= maxfd_) return -1;

  return 0;
}

void CO::Poller::DeleteFD(pollfd *pds, int npds) {
  epoll_event ev;
  pollfd *pd;
  pollfd *epd = pds + npds;
  int old_events, events, op;

  for (pd = pds; pd < epd; ++pd) {
    old_events = GetEvents(pd->fd);

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

int CO::Poller::AddFD(pollfd *pds, int npds) {
  epoll_event ev;
  int i, fd;
  int old_events, events, op;

  for (i = 0; i < npds; i++) {
    fd = pds[i].fd;
    if (fd < 0 || !pds[i].events ||
        (pds[i].events & ~(POLLIN | POLLOUT | POLLPRI))) {
      errno = EINVAL;
      return -1;
    }
    if (fd >= maxfd_)  // fixme
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
      FD_RAWFD(fd) = fd;
      ev.data.ptr = reinterpret_cast<void *>(revtlist_[fd]);

      // evtlist_[fd]->events = events;

      if (epoll_ctl(epfd_, op, fd, &ev) < 0 &&
          (op != EPOLL_CTL_ADD || errno != EEXIST))
        break;
    }
  }

  if (i < npds) {
    /* Error */
    int err = errno;
    /* Unroll the state */
    DeleteFD(pds, i + 1);
    errno = err;
    return -1;
  }

  return 0;
}

int CO::Poller::PrerareCloseFD(int osfd) {
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
  int timeout, nfd, i, osfd, notify;
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

  if (nfd > 0) {
    // fill revents
    for (i = 0; i < nfd; i++) {
      osfd = reinterpret_cast<FDDetail *>(evtlist_[i].data.ptr)->rawfd;
      FD_REVENTS(osfd) = evtlist_[i].events;
      if (FD_REVENTS(osfd) & (EPOLLERR | EPOLLHUP)) {
        /* Also set I/O bits on error */
        FD_REVENTS(osfd) |= GetEvents(osfd);
      }
    }
    // check revents
    // one coroutine from ioq may contain several fds,check it all
    for (q = pinstance->io_queue_.next; q != &pinstance->io_queue_;
         q = q->next) {
      pq = pinstance->GetFromPollerQueue(q);
      notify = 0;
      // how many item on ioq
      epds = pq->pds + pq->npds;

      for (pds = pq->pds; pds < epds; pds++) {
        if (FD_REVENTS(pds->fd) == 0) {
          pds->revents = 0;
          continue;
        }
        osfd = pds->fd;
        events = pds->events;
        revents = 0;
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
          notify = 1;
        }
      }
      // 1 coroutine checked
      if (notify) {
        pinstance->DeleteFromIOQueue(pq);
        pq->on_ioq = 0;
        /*
         * Here we will only delete/modify descriptors that
         * didn't fire (see comments in DeleteFD()).
         */
        DeleteFD(pq->pds, pq->npds);

        if (pq->coroutine->type & CO::Type::OnSleepQueue)
          pinstance->DeleteFromSleepQueue(pq->coroutine);
        pq->coroutine->state = CO::State::KReady;
        pinstance->AddToRunableQueueTail(pq->coroutine);
      }
    }  // all ioq checked

    for (i = 0; i < nfd; i++) {
      /* Delete/modify descriptors that fired */
      osfd = reinterpret_cast<FDDetail *>(evtlist_[i].data.ptr)->rawfd;
      FD_REVENTS(osfd) = 0;
      events = GetEvents(osfd);
      op = events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
      ev.events = events;
      // ev.data.fd = osfd;
      ev.data.ptr = reinterpret_cast<void *>(revtlist_[i]);
      if (epoll_ctl(epfd_, op, osfd, &ev) == 0 && op == EPOLL_CTL_DEL) {
        // epolldata_->evtlist_cnt--;
      }
    }
  }
}
