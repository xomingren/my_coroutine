#include "common.hpp"
#ifdef USE_IOURING

#pragma once
#ifndef __CO_IOURING_HPP__
#define __CO_IOURING_HPP__

#include <liburing.h>  //for io_uring

#include "coroutine.h"

namespace CO {
enum class UringEvent : int { ACCEPT = 1, READ, WRITE };
struct UringDetail {
  int fd;
  UringEvent event;
  bool is_active = false;

  union {
    struct {
      sockaddr_in ipv4_addr;
      socklen_t lens;
    } addr;
    char buf[1024];  // fixme
  };
  io_uring_cqe cqe;
};
class IOuring final {
 private:
  io_uring ring_;
  // fixme how to know the numbers of returned cqe?
  std::unordered_map<int, bool> fd_active_map_;
  int ring_size_ = 10000;

 private:
  IOuring(const IOuring&) = delete;
  IOuring(const IOuring&&) = delete;
  IOuring& operator=(const IOuring&) = delete;
  IOuring& operator=(const IOuring&&) = delete;

 public:
  IOuring() {}
  ~IOuring() { io_uring_queue_exit(&ring_); }

 public:
  int pollerInitOrDie() {
    io_uring_queue_init(ring_size_, &ring_,
                        0 /*flags，0 表示默认配置，例如使用中断驱动模式*/);
    return 1;
  }
  // mark cqe as handled
  void seen(io_uring_cqe* cqe) { io_uring_cqe_seen(&ring_, cqe); }
  // 阻塞式获取请求完成,nonblock:  io_uring_peek_cqe
  int wait(io_uring_cqe** cqe, useconds usec) {
    // if (0 == usec)
    //   return io_uring_wait_cqe(&ring_, cqe);
    // else {
    timespec ts;
    ts.tv_nsec = usec * 1000;
    return io_uring_wait_cqe_timeout(&ring_, cqe, (__kernel_timespec*)&ts);
  }
  // submit all sqe
  // if success return num of sqe submitted, else return errno
  int submit() { return io_uring_submit(&ring_); }

  void accpet_asyn(CO::NetFd* listen_fd) {
    // get all un-submit sqe from ring, one sqe one time
    auto sqe = io_uring_get_sqe(&ring_);
    UringDetail* fd_data =
        reinterpret_cast<UringDetail*>(listen_fd->private_data);
    fd_data->event = UringEvent::ACCEPT;
    fd_data->fd = listen_fd->osfd;
    fd_data->addr.lens = sizeof(sockaddr_in);
    io_uring_prep_accept(sqe, listen_fd->osfd,
                         (sockaddr*)&(fd_data->addr.ipv4_addr),
                         &(fd_data->addr.lens), 0);
    // same with epoll_event->data->ptr
    io_uring_sqe_set_data(sqe, fd_data);
  }

  void read_asyn(CO::NetFd* fd, void* buf, size_t buf_len) {
    auto sqe = io_uring_get_sqe(&ring_);
    UringDetail* fd_data = reinterpret_cast<UringDetail*>(fd->private_data);
    fd_data->event = UringEvent::READ;
    fd_data->fd = fd->osfd;
    io_uring_prep_read(sqe, fd->osfd, buf, buf_len, -1);
    io_uring_sqe_set_data(sqe, fd_data);
  }

  void write_asyn(CO::NetFd* fd, const void* buf, size_t buf_len) {
    auto sqe = io_uring_get_sqe(&ring_);
    UringDetail* fd_data = reinterpret_cast<UringDetail*>(fd->private_data);
    fd_data->event = UringEvent::WRITE;
    fd_data->fd = fd->osfd;
    io_uring_prep_write(sqe, fd->osfd, buf, buf_len, -1);
    io_uring_sqe_set_data(sqe, fd_data);
  }
  int CheckValidFD(int osfd) const noexcept {
    [[unlikely]] if (osfd >= ring_size_)
      return -1;

    return 0;
  }
  void Dispatch() {
    useconds min_utimeout;
    DoubleLinkedList* node;
    PollQueue* pq;
    UringDetail* data;

    int notify;

    CO::Coroutine* pinstance = CO::Coroutine::getInstance();
    // sleep_queue is empty means main coroutine is
    if (nullptr == pinstance->sleep_queue_) {
      min_utimeout = -1;
    } else {
      min_utimeout =
          (pinstance->sleep_queue_->due <= pinstance->last_time_checked)
              ? 0
              : (pinstance->sleep_queue_->due - pinstance->last_time_checked);
      // At least wait 1ms when <1ms, to avoid epoll_wait spin loop.
      if (min_utimeout == 0) {
        min_utimeout = 1000;
      }
    }

    io_uring_cqe* cqe;
    int ret = wait(&cqe, min_utimeout);
    if (ret != 0) return;  // timeout

    unsigned head;
    unsigned count = 0;

    // io_uring_for_each_cqe(&ring_, head, cqe) {
    //   req = reinterpret_cast<UringDetail*>(cqe->user_data);
    //   fd_active_map_[req->fd] = true;
    // }
    // cqe = ring_.cq.khead;
    io_uring_for_each_cqe(&ring_, head, cqe) {
      if (cqe->res == -ENOBUFS) exit(-1);  // should not happend
      ++count;
      data = reinterpret_cast<UringDetail*>(cqe->user_data);
      for (node = pinstance->io_queue_.next; node != &pinstance->io_queue_;
           node = node->next) {
        pq = pinstance->GetFromPollerQueue(node);
        notify = 0;
        for (const auto& i : pq->fdlist) {
          if (i->fd == data->fd) {
            notify = 1;
            data->is_active = true;
            data->cqe = *cqe;
          }
        }
        if (1 == notify) {
          pinstance->DeleteFromIOQueue(pq);
          pq->on_ioq = 0;

          // RemoveEvent(pq->pds, pq->npds);

          if (pq->coroutine->type & CO::Type::OnSleepQueue)
            pinstance->DeleteFromSleepQueue(pq->coroutine);
          pq->coroutine->state = CO::State::kReady;
          pinstance->AddToRunableQueueTail(pq->coroutine);
        }
      }
    }
    io_uring_cq_advance(&ring_, count);
  }
};  // class iouring
}  // namespace CO
#endif  //__CO_IOURING_HPP__

#endif  // use iouring
