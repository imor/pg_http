#include "poller.h"

#ifdef POLLER_USE_EPOLL

#  include <errno.h>
#  include <sys/epoll.h>
#  include <sys/timerfd.h>
#  include <unistd.h>

int poller_create(Poller *poller) {
  poller->fd = epoll_create1(EPOLL_CLOEXEC);

  if (poller->fd < 0) {
    return -1;
  }

  poller->timer_fd = -1;

  return 0;
}

int poller_close(Poller *poller) {
  int res = 0;
  int err = 0;

  if (poller->timer_fd >= 0) {
    res              = close(poller->timer_fd);
    poller->timer_fd = -1;
    err              = errno;
  }

  int res = close(poller->fd);
  poller->fd = -1;

  if (res < 0) {
    return -1;
  }

  if (res < 0) {
    errno = err;
    return -1;
  }

  return 0;
}

/*
 * Arms or disarms the timer, see POLLER_EV_TIMER.
 */
int poller_timer_ctl(Poller *poller, PollerOp op, int timeout_ms) {
  struct itimerspec its = {0};

  // Nothing to disarm if the timer was never set
  if (op == POLLER_OP_DEL && poller->timer_fd < 0) {
    return 0;
  }

  if (op != POLLER_OP_DEL) {
    if (timeout_ms > 0) {
      its.it_value.tv_sec  = timeout_ms / 1000;
      its.it_value.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
    } else {
      // An all-zero it_value disarms the timer, so the closest we can
      // get to "now" is one nanosecond.
      its.it_value.tv_nsec = 1;
    }

    // The timerfd is created and added to the epoll instance when the timer is set for the first
    // time
    if (poller->timer_fd < 0) {
      int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
      if (tfd < 0) {
        return -1;
      }

      struct epoll_event ev = {0};
      ev.events             = EPOLLIN;
      ev.data.fd            = tfd;

      if (epoll_ctl(poller->fd, EPOLL_CTL_ADD, tfd, &ev) < 0) {
        int save_errno = errno;
        close(tfd);
        errno = save_errno;
        return -1;
      }

      poller->timer_fd = tfd;
    }
  }

  return timerfd_settime(poller->timer_fd, 0, &its, NULL);
}

int poller_socket_ctl(Poller *poller, PollerOp op, PollerInterest in) {
  struct epoll_event ev = {0};
  int                cmd;

  switch (op) {
  case POLLER_OP_ADD: cmd = EPOLL_CTL_ADD; break;
  case POLLER_OP_MOD: cmd = EPOLL_CTL_MOD; break;
  case POLLER_OP_DEL: cmd = EPOLL_CTL_DEL; break;
  default           : errno = EINVAL; return -1;
  }

  if (op != POLLER_OP_DEL) {
    if (in.new_events & POLLER_EV_READ) ev.events |= EPOLLIN;
    if (in.new_events & POLLER_EV_WRITE) ev.events |= EPOLLOUT;
    ev.data.fd = in.fd;
  }

  return epoll_ctl(poller->fd, cmd, in.fd, &ev);
}

int poller_wait(Poller *poller, PollerEvent *out, int max_events, int timeout_ms) {
  if (max_events <= 0) {
    errno = EINVAL;
    return -1;
  }

  int n = epoll_wait(poller->fd, out, max_events, timeout_ms);
  if (n < 0) {
    return -1;
  }

  for (int i = 0; i < n; i++) {
    if (out[i].data.fd == poller->timer_fd) {
      // Drain the timerfd so that it is not reported again
      uint64_t expirations;
      ssize_t  r = read(poller->timer_fd, &expirations, sizeof(expirations));
      (void)r;
    }
  }

  return n;
}

int poller_event_fd(const Poller *poller, const PollerEvent *ev) {
  return ev->data.fd == poller->timer_fd ? -1 : ev->data.fd;
}

uint32_t poller_event_kind(const Poller *poller, const PollerEvent *ev) {
  uint32_t e = ev->events, flags = 0;

  if (ev->data.fd == poller->timer_fd) return POLLER_EV_TIMER;

  if (e & EPOLLIN) flags |= POLLER_EV_READ;
  if (e & EPOLLOUT) flags |= POLLER_EV_WRITE;
  if (e & (EPOLLERR | EPOLLHUP)) flags |= POLLER_EV_ERROR;

  return flags;
}

#endif
