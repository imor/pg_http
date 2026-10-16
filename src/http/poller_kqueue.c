#include "poller.h"

#ifdef POLLER_USE_KQUEUE

#  include <errno.h>
#  include <sys/event.h>
#  include <sys/time.h>
#  include <sys/types.h>
#  include <unistd.h>

/*
 * Maximum poller events
 */
#  define POLLER_MAX_EVENTS 256

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
#  define POLLER_TIMER_ID 1

/*
 * Arms or disarms the timer, see POLLER_TIMER.
 */
static int timer_ctl(Poller *poller, PollerOp op, int timeout_ms) {
  struct kevent kev;

  if (op == POLLER_OP_DEL) {
    EV_SET(&kev, POLLER_TIMER_ID, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
  } else if (timeout_ms > 0) {
    // Default unit is milliseconds. EV_ADD on an existing timer resets it.
    EV_SET(&kev, POLLER_TIMER_ID, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, timeout_ms, NULL);
  } else {
    EV_SET(&kev, POLLER_TIMER_ID, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS, 1, NULL);
  }

  if (kevent(poller->fd, &kev, 1, NULL, 0, NULL) < 0) {
    // Disarming a timer which already fired or was never set is not an error
    if (op == POLLER_OP_DEL && errno == ENOENT) return 0;
    return -1;
  }

  return 0;
}

int poller_ctl(Poller *poller, PollerOp op, int fd, PollerEvent *ev) {
  if (ev != NULL && ev->events == POLLER_TIMER) {
    return timer_ctl(poller, op, ev->timeout_ms);
  }

  // ev can be null only when removing interest in an fd, otherwise
  // it is an error
  if (ev == NULL && op != POLLER_OP_DEL) {
    errno = EINVAL;
    return -1;
  }

  uint32_t new_events = ev ? ev->events : POLLER_NONE;
  uint32_t old_events = ev ? ev->old_events : POLLER_ALL;

  struct kevent kevs[2];
  int           n = 0;

  switch (op) {
  case POLLER_OP_ADD: {
    if (new_events & POLLER_READ) EV_SET(&kevs[n++], fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (new_events & POLLER_WRITE) EV_SET(&kevs[n++], fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);

    break;
  }

  case POLLER_OP_MOD: {

    if ((new_events & POLLER_READ) && !(old_events & POLLER_READ)) {
      // If read event is there in new events but not in old, just add it.
      EV_SET(&kevs[n++], fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    } else if (!(new_events & POLLER_READ) && (old_events & POLLER_READ)) {
      // If read event is not there in new events but is there in old, delete it.
      EV_SET(&kevs[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    }

    if ((new_events & POLLER_WRITE) && !(old_events & POLLER_WRITE)) {
      // If write event is there in new events but not in old, just add it.
      EV_SET(&kevs[n++], fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
    } else if (!(new_events & POLLER_WRITE) && (old_events & POLLER_WRITE)) {
      // If write event is not there in new events but is there in old, delete it.
      EV_SET(&kevs[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    }

    break;
  }

  case POLLER_OP_DEL: {
    if (old_events & POLLER_READ) EV_SET(&kevs[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    if (old_events & POLLER_WRITE) EV_SET(&kevs[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);

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
  struct kevent   evs[POLLER_MAX_EVENTS];
  struct timespec ts, *tsp = NULL;

  if (max_events <= 0 || max_events > POLLER_MAX_EVENTS) {
    errno = EINVAL;
    return -1;
  }

  if (timeout_ms >= 0) {
    ts.tv_sec  = timeout_ms / 1000;
    ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
    tsp        = &ts;
  }

  int n = kevent(poller->fd, NULL, 0, evs, max_events, tsp);
  if (n < 0) {
    return -1;
  }

  for (int i = 0; i < n; i++) {
    uint32_t flags = 0;

    if (evs[i].filter == EVFILT_TIMER) {
      out[i].events = POLLER_TIMER;
      out[i].fd     = -1;
      continue;
    }

    if (evs[i].filter == EVFILT_READ) flags |= POLLER_READ;
    if (evs[i].filter == EVFILT_WRITE) flags |= POLLER_WRITE;

    out[i].events = flags;
    out[i].fd     = (int)evs[i].ident;
  }

  return n;
}

#endif
