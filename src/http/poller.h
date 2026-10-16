#ifndef PG_HTTP_POLLER_H
#define PG_HTTP_POLLER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __linux__
#  define POLLER_USE_EPOLL
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#  define POLLER_USE_KQUEUE
#else
#  error "No I/O multiplexing API available on this OS"
#endif

/*
 * Flags indicating the event type.
 */
#define POLLER_NONE 0
#define POLLER_READ 1
#define POLLER_WRITE 2
#define POLLER_ALL (POLLER_READ | POLLER_WRITE)

/*
 * Selects the poller's single one-shot timer instead of an fd. When passed
 * to poller_ctl, it must be the only flag set in the event and the `fd`
 * argument is ignored. POLLER_OP_ADD and POLLER_OP_MOD both arm the timer
 * (replacing a previously armed one) to expire after `timeout_ms`, and
 * POLLER_OP_DEL disarms it. When returned by poller_wait it means the timer
 * has expired, and the `fd` of such an event is -1.
 */
#define POLLER_TIMER 4

/*
 * The main poller struct containing the root file descriptor for the
 * multiplexing API (epoll or kqueue)
 */
typedef struct Poller {
  // The root fd
  int fd;

#ifdef POLLER_USE_EPOLL
  // timerfd registered with the epoll instance to implement the timer.
  // kqueue has native timers and needs no extra state.
  int timer_fd;

  // Whether timer_fd has been added to the epoll instance. This happens
  // lazily the first time the timer is set.
  bool timer_registered;
#endif
} Poller;

// TODO: Consider using a typedef to epoll_event and kevent instead
// of a new PollerEvent struct to avoid copying data.

/*
 * Event accepted by either poller_ctl to update interest for
 * one fd, or returned by poller_wait in the out array to indicate
 * the fds which are readable or writable.
 */
typedef struct PollerEvent {
  // A combination of POLLER_READ and POLLER_WRITE
  uint32_t events;

  // fd with which this fd is associated
  int fd;

  // Only used with POLLER_TIMER: milliseconds until the timer expires. Zero
  // means as soon as possible.
  int timeout_ms;

#ifdef POLLER_USE_KQUEUE
  // Old events which were registered with the kernel. This is needed only
  // for kqueue impl because of the way the kqueue API works. It has a
  // separate filter for read and write events due to which it's not enough
  // to know the new events only. For example, if a read event was added
  // in the past and now the new event is a write event, we'll need to
  // remove the old read event and then add a write event. If we do not
  // know that a read event was already added in the past, we can't remove
  // it. `old_event` stores that past history.
  //
  // This is not a problem with epoll API because it replaces all events
  // for an fd in a single epoll_ctl call.
  uint32_t old_events;
#endif
} PollerEvent;

typedef enum PollerOp {
  // Add the fd to the interest list
  POLLER_OP_ADD,

  // Modified the fd in the interest list
  POLLER_OP_MOD,

  // Delete the fd from the interest list
  POLLER_OP_DEL
} PollerOp;

/*
 * Initializes a new poller instance by setting its fd to
 * a file descriptor created by the mux API. Returns zero on
 * success and -1 on error. errno is set on error.
 */
int poller_create(Poller *poller);

/*
 * Closes a poller instance. Returns zero on success
 * and -1 on error. errno is set on error.
 */
int poller_close(Poller *poller);

/*
 * Adds, modified or deletes interest in an fd to the poller.
 * The ev indicates what kind of event is the caller interested in.
 *
 * When op is POLLER_OP_DEL, ev is ignored in epoll and can be null,
 * otherwise it must be set to a non-null event. For kqueue ev must
 * never be null and for POLLER_OP_MOD and POLLER_OP_DEL `old_events`
 * must be set.
 *
 * Returns zero on success and -1 on error. errno is set on error.
 */
int poller_ctl(Poller *poller, PollerOp op, int fd, PollerEvent *ev);

/*
 * Returns a list of events in `out` indicating potentially readable
 * or writable fds. If it is -1 it blocks until an event is available.
 * If it zero or positive, it waits for a maximum of that many
 * milliseconds before returning. Zero makes it return immediately
 *
 * On success returns the number of items placed in the `out` array.
 * On error returns -1. errno is set on error.
 */
int poller_wait(Poller *poller, PollerEvent *out, int max_events, int timeout_ms);

#endif /* PG_HTTP_POLLER_H */
