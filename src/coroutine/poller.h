#pragma once
#ifndef __CO_POLLER_H__
#define __CO_POLLER_H__

#include <vector>

#include "common.hpp"

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
    FDDetail()
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
  // about fd, every process had an individual fd-table, start with 0
  // 0-stdin 1-stdout 2-stderr ,we might use from 3
  int GetEvents(int fd) const;

 public:
  int pollerInitOrDie();

  int CheckValidFD(int osfd) const;

  void DeleteFD(pollfd *pds, int npds);

  int AddFD(pollfd *pds, int npds);

  int PrerareCloseFD(int osfd);

  void Dispatch(void);
};  // class Poller
}  // namespace CO
#endif  //__CO_POLLER_H__