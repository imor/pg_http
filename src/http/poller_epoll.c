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

  poller->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (poller->timer_fd < 0) {
    int save_errno = errno;
    close(poller->fd);
    poller->fd = -1;
    errno      = save_errno;
    return -1;
  }

  poller->timer_registered = false;

  return 0;
}

int poller_close(Poller *poller) {
  int res = close(poller->timer_fd);
  int err = errno;

  poller->timer_fd = -1;

  if (close(poller->fd) < 0) {
    return -1;
  }

  poller->fd = -1;

  if (res < 0) {
    errno = err;
    return -1;
  }

  return 0;
}

/*
 * Arms or disarms the timer, see POLLER_TIMER.
 */
static int timer_ctl(Poller *poller, PollerOp op, int timeout_ms) {
  struct itimerspec its = {0};

  if (op != POLLER_OP_DEL) {
    if (timeout_ms > 0) {
      its.it_value.tv_sec  = timeout_ms / 1000;
      its.it_value.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
    } else {
      // An all-zero it_value disarms the timer, so the closest we can
      // get to "now" is one nanosecond.
      its.it_value.tv_nsec = 1;
    }

    // The timerfd is added to the epoll instance when the timer is set for the first time
    if (!poller->timer_registered) {
      struct epoll_event ev = {0};
      ev.events             = EPOLLIN;
      ev.data.fd            = poller->timer_fd;

      if (epoll_ctl(poller->fd, EPOLL_CTL_ADD, poller->timer_fd, &ev) < 0) {
        return -1;
      }

      poller->timer_registered = true;
    }
  }

  return timerfd_settime(poller->timer_fd, 0, &its, NULL);
}

int poller_ctl(Poller *poller, PollerOp op, const PollerInterest *in) {
  if (in == NULL) {
    errno = EINVAL;
    return -1;
  }

  if ((in->new_events | in->old_events) & POLLER_TIMER) {
    return timer_ctl(poller, op, in->timeout_ms);
  }

  struct epoll_event ev = {0};
  int                cmd;

  switch (op) {
  case POLLER_OP_ADD: cmd = EPOLL_CTL_ADD; break;
  case POLLER_OP_MOD: cmd = EPOLL_CTL_MOD; break;
  case POLLER_OP_DEL: cmd = EPOLL_CTL_DEL; break;
  default           : errno = EINVAL; return -1;
  }

  if (op != POLLER_OP_DEL) {
    if (in->new_events & POLLER_READ) ev.events |= EPOLLIN;
    if (in->new_events & POLLER_WRITE) ev.events |= EPOLLOUT;
    ev.data.fd = in->fd;
  }

  return epoll_ctl(poller->fd, cmd, in->fd, &ev);
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

uint32_t poller_event_flags(const Poller *poller, const PollerEvent *ev) {
  uint32_t e = ev->events, flags = 0;

  if (ev->data.fd == poller->timer_fd) return POLLER_TIMER;

  if (e & EPOLLIN) flags |= POLLER_READ;
  if (e & EPOLLOUT) flags |= POLLER_WRITE;
  if (e & (EPOLLERR | EPOLLHUP)) flags |= POLLER_READ | POLLER_WRITE;

  return flags;
}

#endif
