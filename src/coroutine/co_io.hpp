#pragma once
#ifndef __CO_IO_HPP__
#define __CO_IO_HPP__

#include <errno.h>
#include <fcntl.h>      //for fcntl
#include <poll.h>       //for pollfd
#include <sys/ioctl.h>  //for octl
#include <sys/socket.h>
#include <sys/uio.h>  //for iovec

#include "common.hpp"
#include "coroutine.h"
#include "poller.h"

namespace CO {

NetFd *newNetFD(int osfd, int nonblock, int is_socket) {
  NetFd *fd;
  int flags = 1;

  auto p = Coroutine::getInstance();
  if (p->get_poller()->CheckValidFD(osfd) < 0) return nullptr;
  fd = reinterpret_cast<NetFd *>(calloc(1, sizeof(NetFd)));
  if (!fd) return NULL;

  fd->osfd = osfd;
  fd->inuse = 1;
  fd->next = NULL;

  if (nonblock) {
    /* Use just one system call */
    if (is_socket && ioctl(osfd, FIONBIO, &flags) != -1) return fd;
    /* Do it the Posix way */
    if ((flags = fcntl(osfd, F_GETFL, 0)) < 0 ||
        fcntl(osfd, F_SETFL, flags | O_NONBLOCK) < 0) {
      return NULL;
    }
  }

  return fd;
}

int closeNetFD(NetFd *fd) {
  auto p = Coroutine::getInstance();
  if (p->get_poller()->PrerareCloseFD(fd->osfd) < 0) return -1;
  return close(fd->osfd);
}

int NetFdPoll(CO::NetFd *fd, int how, CO::useconds timeout) {
  pollfd pd;
  int n;

  pd.fd = fd->osfd;
  pd.events = (short)how;
  pd.revents = 0;

  auto p = Coroutine::getInstance();
  if ((n = p->Poll(&pd, 1, timeout)) < 0) return -1;
  if (n == 0) {
    /* Timed out */
    errno = ETIME;
    return -1;
  }
  if (pd.revents & POLLNVAL) {
    errno = EBADF;
    return -1;
  }

  return 0;
}

NetFd *co_accept(NetFd *fd, sockaddr *addr, int *addrlen, useconds timeout) {
  int osfd, err;
  NetFd *newfd;

  while ((osfd = accept(fd->osfd, addr, (socklen_t *)addrlen)) < 0) {
    if (errno == EINTR) continue;
    if (!((errno == EAGAIN) || (errno == EWOULDBLOCK))) return NULL;
    /* Wait until the socket becomes readable */
    if (NetFdPoll(fd, POLLIN, timeout) < 0) return NULL;
  }
  // #if defined(MD_ACCEPT_NB_INHERITED)
  //   newfd = newNetFD(osfd, 0, 1);
  // #elif defined(MD_ACCEPT_NB_NOT_INHERITED)
  //   newfd = newNetFD(osfd, 1, 1);
  // #endif
  newfd = newNetFD(osfd, 1, 1);
  if (nullptr == newfd) {
    err = errno;
    close(osfd);
    errno = err;
  }

  return newfd;
}
int co_connect(NetFd *fd, const sockaddr *addr, int addrlen, useconds timeout) {
  int n, err = 0;

  while (connect(fd->osfd, addr, addrlen) < 0) {
    if (errno != EINTR) {
      /*
       * On some platforms, if connect() is interrupted (errno == EINTR)
       * after the kernel binds the socket, a subsequent connect()
       * attempt will fail with errno == EADDRINUSE.  Ignore EADDRINUSE
       * iff connect() was previously interrupted.  See Rich Stevens'
       * "UNIX Network Programming," Vol. 1, 2nd edition, p. 413
       * ("Interrupted connect").
       */
      if (errno != EINPROGRESS && (errno != EADDRINUSE || err == 0)) return -1;
      /* Wait until the socket becomes writable */
      if (NetFdPoll(fd, POLLOUT, timeout) < 0) return -1;
      /* Try to find out whether the connection setup succeeded or failed */
      n = sizeof(int);
      if (getsockopt(fd->osfd, SOL_SOCKET, SO_ERROR, (char *)&err,
                     (socklen_t *)&n) < 0)
        return -1;
      if (err) {
        errno = err;
        return -1;
      }
      break;
    }
    err = 1;
  }

  return 0;
}

ssize_t co_read(NetFd *fd, void *buf, size_t nbyte, useconds timeout) {
  ssize_t n;

  while ((n = read(fd->osfd, buf, nbyte)) < 0) {
    if (errno == EINTR) continue;
    if (!((errno == EAGAIN) || (errno == EWOULDBLOCK))) return -1;

    /* Wait until the socket becomes readable */
    if (NetFdPoll(fd, POLLIN, timeout) < 0) return -1;
  }

  return n;
}

ssize_t co_readv(NetFd *fd, const iovec *iov, int iov_size, useconds timeout) {
  ssize_t n;

  while ((n = readv(fd->osfd, iov, iov_size)) < 0) {
    if (errno == EINTR) continue;
    if (!((errno == EAGAIN) || (errno == EWOULDBLOCK))) return -1;

    /* Wait until the socket becomes readable */
    if (NetFdPoll(fd, POLLIN, timeout) < 0) return -1;
  }

  return n;
}
int st_writev_resid(NetFd *fd, iovec **iov, int *iov_size, useconds timeout) {
  ssize_t n;

  while (*iov_size > 0) {
    if (*iov_size == 1)
      n = write(fd->osfd, (*iov)->iov_base, (*iov)->iov_len);
    else
      n = writev(fd->osfd, *iov, *iov_size);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (!((errno == EAGAIN) || (errno == EWOULDBLOCK))) return -1;
    } else {
      while ((size_t)n >= (*iov)->iov_len) {
        n -= (*iov)->iov_len;
        (*iov)->iov_base = (char *)(*iov)->iov_base + (*iov)->iov_len;
        (*iov)->iov_len = 0;
        (*iov)++;
        (*iov_size)--;
        if (n == 0) break;
      }
      if (*iov_size == 0) break;
      (*iov)->iov_base = (char *)(*iov)->iov_base + n;
      (*iov)->iov_len -= n;
    }

    /* Wait until the socket becomes writable */
    if (NetFdPoll(fd, POLLOUT, timeout) < 0) return -1;
  }

  return 0;
}
int st_write_resid(NetFd *fd, const void *buf, size_t *resid,
                   useconds timeout) {
  iovec iov, *riov;
  int riov_size, rv;

  iov.iov_base = (void *)buf; /* we promise not to modify buf */
  iov.iov_len = *resid;
  riov = &iov;
  riov_size = 1;
  rv = st_writev_resid(fd, &riov, &riov_size, timeout);
  *resid = iov.iov_len;
  return rv;
}
ssize_t co_write(NetFd *fd, const void *buf, size_t nbyte, useconds timeout) {
  size_t resid = nbyte;
  return st_write_resid(fd, buf, &resid, timeout) == 0
             ? (ssize_t)(nbyte - resid)
             : -1;
}
ssize_t co_writev(NetFd *fd, const iovec *iov, int iov_size, useconds timeout) {
  ssize_t n, rv;
  size_t nleft, nbyte;
  int index, iov_cnt;
  iovec *tmp_iov;
  iovec local_iov[kLocalMaxIOV];

  /* Calculate the total number of bytes to be sent */
  nbyte = 0;
  for (index = 0; index < iov_size; index++) nbyte += iov[index].iov_len;

  rv = (ssize_t)nbyte;
  nleft = nbyte;
  tmp_iov = (iovec *)iov; /* we promise not to modify iov */
  iov_cnt = iov_size;

  while (nleft > 0) {
    if (iov_cnt == 1) {
      if (co_write(fd, tmp_iov[0].iov_base, nleft, timeout) != (ssize_t)nleft)
        rv = -1;
      break;
    }
    if ((n = writev(fd->osfd, tmp_iov, iov_cnt)) < 0) {
      if (errno == EINTR) continue;
      if (!((errno == EAGAIN) || (errno == EWOULDBLOCK))) {
        rv = -1;
        break;
      }
    } else {
      if ((size_t)n == nleft) break;
      nleft -= n;
      /* Find the next unwritten vector */
      n = (ssize_t)(nbyte - nleft);
      for (index = 0; (size_t)n >= iov[index].iov_len; index++)
        n -= iov[index].iov_len;

      if (tmp_iov == iov) {
        /* Must copy iov's around */
        if (iov_size - index <= kLocalMaxIOV) {
          tmp_iov = local_iov;
        } else {
          tmp_iov = reinterpret_cast<iovec *>(
              calloc(1, (iov_size - index) * sizeof(iovec)));
          if (tmp_iov == NULL) return -1;
        }
      }

      /* Fill in the first partial read */
      tmp_iov[0].iov_base = &(((char *)iov[index].iov_base)[n]);
      tmp_iov[0].iov_len = iov[index].iov_len - n;
      index++;
      /* Copy the remaining vectors */
      for (iov_cnt = 1; index < iov_size; iov_cnt++, index++) {
        tmp_iov[iov_cnt].iov_base = iov[index].iov_base;
        tmp_iov[iov_cnt].iov_len = iov[index].iov_len;
      }
    }

    /* Wait until the socket becomes writable */
    if (NetFdPoll(fd, POLLOUT, timeout) < 0) {
      rv = -1;
      break;
    }
  }

  if (tmp_iov != iov && tmp_iov != local_iov) free(tmp_iov);

  return rv;
}
}  // namespace CO
#endif  //__CO_IO_HPP__