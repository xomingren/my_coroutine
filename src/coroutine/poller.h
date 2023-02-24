#include "common.hpp"
#ifdef USE_EPOLL
#pragma once
#ifndef __CO_POLLER_H__
#define __CO_POLLER_H__

class epoll_event;
class pollfd;
namespace CO {
class Poller final {
 public:
  Poller();
  ~Poller();  // fixme
 private:
  Poller(const Poller &) = delete;
  Poller(const Poller &&) = delete;
  Poller &operator=(const Poller &) = delete;
  Poller &operator=(const Poller &&) = delete;

 private:
  struct FDDetail {
    FDDetail() noexcept
        : rd_ref_cnt(0), wr_ref_cnt(0), ex_ref_cnt(0), revents(0), rawfd(-1) {}
    int rd_ref_cnt;
    int wr_ref_cnt;
    int ex_ref_cnt;
    int revents;
    int rawfd;
  };  // struct FDDetail
  int maxfd_;
  int epfd_;
  std::vector<epoll_event> evtlist_;
  std::vector<FDDetail *> revtlist_;

 private:
  int GetEvents(int fd) const noexcept;

 public:
  [[nodiscard]] int pollerInitOrDie();

  [[nodiscard]] int CheckValidFD(int osfd) const noexcept;

  void RemoveEvent(pollfd *pds, int npds);

  [[nodiscard]] int RegisterEvent(pollfd *pds, int npds);

  [[nodiscard]] int PrepareCloseFD(int osfd) const noexcept;

  void Dispatch(void);
};  // class Poller
}  // namespace CO
#endif  //__CO_POLLER_H__
#endif  // use EPOLL
