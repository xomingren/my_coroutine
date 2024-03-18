#include "common.hpp"
#ifdef USE_IOURING

#pragma once
#ifndef __CO_IOURING_HPP__
#define __CO_IOURING_HPP__

#include <liburing.h>  //for io_uring

// #include "coroutine.h"

namespace CO {
enum class UringEvent : int {
  DEFAULT = -1,
  ACCEPT = 1,
  READ = 2,
  WRITE = 3,
  WAKE
};
struct UringDetail {
  ~UringDetail() { std::cout << "out UringDetail" << std::endl; }
  int fd = -1;
  UringEvent event = UringEvent::DEFAULT;
  bool is_active = false;
  int result_code = -1;
};  // struct UringDetail
class IOuring final {
 private:
  int ring_size_ = kMaxFD;
  io_uring ring_;
  // fixme how to know the numbers of returned cqe?
  std::unordered_map<int, bool> fd_active_map_;

 public:
  IOuring() {}
  ~IOuring() {
    // std::cout << "out class IOuring" << std::endl;
    io_uring_queue_exit(&ring_);
  }

 private:
  IOuring(const IOuring&) = delete;
  IOuring(const IOuring&&) = delete;
  IOuring& operator=(const IOuring&) = delete;
  IOuring& operator=(const IOuring&&) = delete;

 public:
  int pollerInitOrDie() {
    return io_uring_queue_init(
        ring_size_, &ring_, 0 /*flags，0 表示默认配置，例如使用中断驱动模式*/);
  }
  // mark cqe as handled
  void seen(io_uring_cqe* cqe) { io_uring_cqe_seen(&ring_, cqe); }
  void seen_many(size_t nums) { io_uring_cq_advance(&ring_, nums); }
  // get cqe in block-way, nonblock: io_uring_peek_cqe
  int wait(io_uring_cqe** cqe, useconds usec) {
    // if (0 == usec)
    //   return io_uring_wait_cqe(&ring_, cqe);
    // else {
    timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = usec * 1000;
    return io_uring_wait_cqe_timeout(&ring_, cqe, (__kernel_timespec*)&ts);
  }
  // submit all sqe
  // if success return num of sqe submitted, else return errno
  int submit() { return io_uring_submit(&ring_); }

  void accpet_asyn(NetFD* listen_fd, sockaddr* addr, uint* addrlen) {
    // get all un-submit sqe from ring, one sqe one time
    auto sqe = io_uring_get_sqe(&ring_);
    auto fd_data = listen_fd->private_data;
    fd_data->event = UringEvent::ACCEPT;
    fd_data->fd = listen_fd->osfd;
    io_uring_prep_accept(sqe, listen_fd->osfd, addr,
                         reinterpret_cast<socklen_t*>(addrlen), 0);
    // same with epoll_event->data->ptr
    io_uring_sqe_set_data(sqe, fd_data.get());
  }

  void read_asyn(NetFD* fd, void* buf, size_t buf_len) {
    auto sqe = io_uring_get_sqe(&ring_);
    auto fd_data = fd->private_data;
    fd_data->event = UringEvent::READ;
    fd_data->fd = fd->osfd;
    io_uring_prep_read(sqe, fd->osfd, buf, buf_len, -1);
    io_uring_sqe_set_data(sqe, fd_data.get());
  }

  void write_asyn(NetFD* fd, const void* buf, size_t buf_len) {
    auto sqe = io_uring_get_sqe(&ring_);
    auto fd_data = fd->private_data;
    fd_data->event = UringEvent::WRITE;
    fd_data->fd = fd->osfd;
    io_uring_prep_write(sqe, fd->osfd, buf, buf_len, -1);
    io_uring_sqe_set_data(sqe, fd_data.get());
  }
  int CheckValidFD(int osfd) const noexcept {
    [[unlikely]] if (osfd >= ring_size_)
      return -1;

    return 0;
  }

  int PrepareCloseFD(NetFD* fd) const noexcept {
    if (auto data = fd->private_data; data->event != UringEvent::DEFAULT) {
      errno = EBUSY;
      return -1;
    }
    return 0;
  }

  void setWakeup(int wakeup_fd) {
    UringDetail* fd_data = new UringDetail;  // delete on dispatch() WAKE
    auto sqe = io_uring_get_sqe(&ring_);
    fd_data->event = UringEvent::WAKE;
    fd_data->fd = wakeup_fd;
    uint64_t one;
    io_uring_prep_rw(IORING_OP_READ, sqe, wakeup_fd, &one, sizeof one, -1);
    io_uring_sqe_set_data(sqe, fd_data);
  }

  void Dispatch() {
    useconds min_utimeout;

    CO::Coroutine* pinstance = CO::Coroutine::getInstance();
    if (nullptr == pinstance->sleep_queue_) {
      min_utimeout = kNerverTimeout;  // ULLONG_MAX
    } else {
      min_utimeout =
          (pinstance->sleep_queue_->due <= pinstance->last_time_checked)
              ? 0
              : (pinstance->sleep_queue_->due - pinstance->last_time_checked);
      // At least wait 1ms when <1ms, to avoid spin loop.
      if (min_utimeout == 0) {
        min_utimeout = 1000;
      }
    }

    io_uring_cqe* cqe;
    int ret = wait(&cqe, min_utimeout);

    if (ret != 0)
      return;  // timeout set by sleep_queue_, for each single fd, use
               // setsockopt()

    unsigned head;
    unsigned count = 0;

    // io_uring_for_each_cqe(&ring_, head, cqe) {
    //   req = reinterpret_cast<UringDetail*>(cqe->user_data);
    //   fd_active_map_[req->fd] = true;
    // }
    // cqe = ring_.cq.khead;
    UringDetail* data;
    PollQueue* pq;
    bool activated;
    io_uring_for_each_cqe(&ring_, head, cqe) {
      if (cqe->res == -ENOBUFS) exit(-1);  // should not happend
      if (cqe->res >= 0) {
        ++count;
        data = reinterpret_cast<UringDetail*>(cqe->user_data);
        [[unlikely]] if (data->event == UringEvent::WAKE) {
          delete data;
          for (DoubleLinkedList* node = pinstance->io_queue_.next;
               node != &pinstance->io_queue_; node = node->next) {
            auto pq = pinstance->GetFromPollerQueue(node);
            pq->on_ioq = -1;
            pinstance->DeleteFromIOQueue(pq);
            if (pq->coroutine->type & CO::Type::OnSleepQueue)
              pinstance->DeleteFromSleepQueue(pq->coroutine);
            pq->coroutine->state = CO::State::kReady;
            pinstance->InsertAfterRunableQueueHead(pq->coroutine.get());
          }
        } else {
          for (DoubleLinkedList* node = pinstance->io_queue_.next;
               node != &pinstance->io_queue_; node = node->next) {
            pq = pinstance->GetFromPollerQueue(node);
            activated = false;
            for (const auto& i : pq->fdlist) {
              if (i->fd == data->fd) {
                activated = true;
                data->is_active = true;
                data->result_code = cqe->res;
              }
            }
            if (activated) {
              pinstance->DeleteFromIOQueue(pq);
              pq->on_ioq = 0;
              if (pq->coroutine->type & CO::Type::OnSleepQueue)
                pinstance->DeleteFromSleepQueue(pq->coroutine);
              pq->coroutine->state = CO::State::kReady;
              pinstance->AddToRunableQueueTail(pq->coroutine.get());
            }
          }
        }
      }
    }
    seen_many(count);
  }
};  // class iouring
}  // namespace CO
#endif  //__CO_IOURING_HPP__

#endif  // use iouring
