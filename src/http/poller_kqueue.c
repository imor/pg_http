#include "poller.h"

#ifdef POLLER_USE_KQUEUE

#  include <errno.h>
#  include <unistd.h>

int poller_create(Poller *poller) {
  poller->fd = kqueue();

  if (poller->fd < 0) {
    return -1;
  }

  return 0;
}

/*
 * Id of the timer when used in the `EV_SET` call. This can be the same
 * as another fd because in kqueue a kevent is uniquely identified by
 * the id and filter pair. So a (1, `EVFILT_TIMER`) is different from
 * (1, `EVFILT_READ`).
 */
#  define TIMER_ID 1

int poller_timer_ctl(Poller *poller, PollerOp op, int timeout_ms) {
  struct kevent kev;

  switch (op) {
  case POLLER_OP_ADD:
  case POLLER_OP_MOD: {
    // Add and modify are the same because kqueue just replaces the timeout filter
    //
    // We use EV_DISPATCH intead of EV_ONESHOT because the latter deletes the
    // timer once it fires, which is problematic for our delete kevent call later
    // because it will error with ENOENT which we would have to ignore. The former
    // keeps the timer alive but just disables it, making the delete kevent call
    // succeed without ENOENT.
    EV_SET(&kev, TIMER_ID, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_DISPATCH, 0, timeout_ms, NULL);
    break;
  }
  case POLLER_OP_DEL: {
    EV_SET(&kev, TIMER_ID, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
    break;
  }
  default: {
    errno = EINVAL;
    return -1;
    break;
  }
  }

  return kevent(poller->fd, &kev, 1, NULL, 0, NULL);
}

int poller_socket_ctl(Poller *poller, PollerOp op, PollerInterest in) {
  int      fd         = in.fd;
  uint32_t new_events = in.new_events;
  uint32_t old_events = in.old_events;

  struct kevent kevs[2];
  int           n = 0;

  switch (op) {
  case POLLER_OP_ADD: {
    if (new_events & POLLER_EV_READ) EV_SET(&kevs[n++], fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (new_events & POLLER_EV_WRITE) EV_SET(&kevs[n++], fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);

    break;
  }

  case POLLER_OP_MOD: {

    if ((new_events & POLLER_EV_READ) && !(old_events & POLLER_EV_READ)) {
      // If read event is there in new events but not in old, just add it.
      EV_SET(&kevs[n++], fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    } else if (!(new_events & POLLER_EV_READ) && (old_events & POLLER_EV_READ)) {
      // If read event is not there in new events but is there in old, delete it.
      EV_SET(&kevs[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    }

    if ((new_events & POLLER_EV_WRITE) && !(old_events & POLLER_EV_WRITE)) {
      // If write event is there in new events but not in old, just add it.
      EV_SET(&kevs[n++], fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
    } else if (!(new_events & POLLER_EV_WRITE) && (old_events & POLLER_EV_WRITE)) {
      // If write event is not there in new events but is there in old, delete it.
      EV_SET(&kevs[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    }

    break;
  }

  case POLLER_OP_DEL: {
    if (old_events & POLLER_EV_READ) EV_SET(&kevs[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    if (old_events & POLLER_EV_WRITE) EV_SET(&kevs[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);

    break;
  }

  default: errno = EINVAL; return -1;
  }

  if (n == 0) {
    return 0;
  }

  return kevent(poller->fd, kevs, n, NULL, 0, NULL);
}

int poller_wait(Poller *poller, PollerEvent *out, int max_events, int timeout_ms) {
  struct timespec  ts;
  struct timespec *tsp = NULL;

  if (timeout_ms >= 0) {
    ts.tv_sec  = timeout_ms / 1000;
    ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
    tsp        = &ts;
  }

  return kevent(poller->fd, NULL, 0, out, max_events, tsp);
}

int poller_close(Poller *poller) {
  int res    = close(poller->fd);
  poller->fd = -1;

  if (res < 0) {
    return -1;
  }

  return 0;
}

int poller_event_fd(__attribute__((unused)) const Poller *poller, const PollerEvent *ev) {
  return ev->filter == EVFILT_TIMER ? -1 : (int)ev->ident;
}

uint32_t poller_event_kind(__attribute__((unused)) const Poller *poller, const PollerEvent *ev) {
  uint32_t flags = POLLER_EV_NONE;

  if (ev->filter == EVFILT_TIMER) return POLLER_EV_TIMER;
  if (ev->filter == EVFILT_READ) flags |= POLLER_EV_READ;
  if (ev->filter == EVFILT_WRITE) flags |= POLLER_EV_WRITE;
  if ((ev->flags & EV_EOF) && ev->fflags != 0) flags |= POLLER_EV_ERROR;

  return flags;
}

#endif
