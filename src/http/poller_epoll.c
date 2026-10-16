#include "poller.h"

#ifdef POLLER_USE_EPOLL

#  include <errno.h>
#  include <sys/epoll.h>
#  include <unistd.h>

/*
 * Maximum poller events
 */
#  define POLLER_MAX_EVENTS 256

int poller_create(Poller *poller) {
  poller->fd = epoll_create1(EPOLL_CLOEXEC);

  if (poller->fd < 0) {
    return -1;
  }

  return 0;
}

int poller_close(Poller *poller) {
  int res = close(poller->fd);

  if (res < 0) {
    return -1;
  }

  poller->fd = -1;
  return 0;
}

int poller_ctl(Poller *poller, PollerOp op, int fd, PollerEvent *event) {
  struct epoll_event ev = {0};
  int                cmd;

  switch (op) {
  case POLLER_OP_ADD: cmd = EPOLL_CTL_ADD; break;
  case POLLER_OP_MOD: cmd = EPOLL_CTL_MOD; break;
  case POLLER_OP_DEL: cmd = EPOLL_CTL_DEL; break;
  default           : errno = EINVAL; return -1;
  }

  if (op != POLLER_OP_DEL) {
    if (event == NULL) {
      errno = EINVAL;
      return -1;
    }
    if (event->events & POLLER_READ) ev.events |= EPOLLIN;
    if (event->events & POLLER_WRITE) ev.events |= EPOLLOUT;
    ev.data.fd = fd;
  }

  return epoll_ctl(poller->fd, cmd, fd, &ev);
}

int poller_wait(Poller *poller, PollerEvent *out, int max_events, int timeout_ms) {
  if (max_events <= 0 || max_events > POLLER_MAX_EVENTS) {
    errno = EINVAL;
    return -1;
  }

  struct epoll_event evs[POLLER_MAX_EVENTS];

  int n = epoll_wait(poller->fd, evs, max_events, timeout_ms);
  if (n < 0) {
    return -1;
  }

  for (int i = 0; i < n; i++) {
    uint32_t e = evs[i].events, flags = 0;

    if (e & EPOLLIN) flags |= POLLER_READ;
    if (e & EPOLLOUT) flags |= POLLER_WRITE;
    if (e & (EPOLLERR | EPOLLHUP)) flags |= POLLER_READ | POLLER_WRITE;

    out[i].events = flags;
    out[i].fd     = evs[i].data.fd;
  }

  return n;
}

#endif
