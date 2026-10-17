#include "poller.h"

#ifdef POLLER_USE_KQUEUE

#  include <errno.h>
#  include <sys/event.h>
#  include <sys/time.h>
#  include <sys/types.h>
#  include <unistd.h>

int poller_create(Poller *poller) {
  poller->fd = kqueue();

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

/*
 * Identifier of the one and only timer. Timer idents live in a separate
 * namespace from fds so there is no clash.
 */
#  define POLLER_EV_TIMER_ID 1

/*
 * Arms or disarms the timer, see POLLER_EV_TIMER.
 */
static int timer_ctl(Poller *poller, PollerOp op, int timeout_ms) {
  struct kevent kev;

  if (op == POLLER_OP_DEL) {
    EV_SET(&kev, POLLER_EV_TIMER_ID, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
  } else if (timeout_ms > 0) {
    // Default unit is milliseconds. EV_ADD on an existing timer resets it.
    EV_SET(&kev, POLLER_EV_TIMER_ID, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, timeout_ms, NULL);
  } else {
    EV_SET(&kev, POLLER_EV_TIMER_ID, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS, 1, NULL);
  }

  if (kevent(poller->fd, &kev, 1, NULL, 0, NULL) < 0) {
    // Disarming a timer which already fired or was never set is not an error
    if (op == POLLER_OP_DEL && errno == ENOENT) return 0;
    return -1;
  }

  return 0;
}

int poller_ctl(Poller *poller, PollerOp op, const PollerInterest *in) {
  if (in == NULL) {
    errno = EINVAL;
    return -1;
  }

  if ((in->new_events | in->old_events) & POLLER_EV_TIMER) {
    return timer_ctl(poller, op, in->timeout_ms);
  }

  int      fd         = in->fd;
  uint32_t new_events = in->new_events;
  uint32_t old_events = in->old_events;

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

  return kevent(poller->fd, kevs, n, NULL, 0, NULL) < 0 ? -1 : 0;
}

int poller_wait(Poller *poller, PollerEvent *out, int max_events, int timeout_ms) {
  struct timespec ts, *tsp = NULL;

  if (max_events <= 0) {
    errno = EINVAL;
    return -1;
  }

  if (timeout_ms >= 0) {
    ts.tv_sec  = timeout_ms / 1000;
    ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
    tsp        = &ts;
  }

  return kevent(poller->fd, NULL, 0, out, max_events, tsp);
}

int poller_event_fd(__attribute__((unused)) const Poller *poller, const PollerEvent *ev) {
  return ev->filter == EVFILT_TIMER ? -1 : (int)ev->ident;
}

uint32_t poller_event_kind(__attribute__((unused)) const Poller *poller, const PollerEvent *ev) {
  if (ev->filter == EVFILT_TIMER) return POLLER_EV_TIMER;
  if (ev->filter == EVFILT_READ) return POLLER_EV_READ;
  if (ev->filter == EVFILT_WRITE) return POLLER_EV_WRITE;
  return POLLER_EV_NONE;
}

#endif
