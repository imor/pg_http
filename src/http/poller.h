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

#ifdef POLLER_USE_EPOLL
#  include <sys/epoll.h>
#else
#  include <sys/event.h>
#endif

/*
 * Flags indicating the event type.
 */
#define POLLER_NONE 0
#define POLLER_READ 1
#define POLLER_WRITE 2
#define POLLER_ALL (POLLER_READ | POLLER_WRITE)

/*
 * Selects the poller's single one-shot timer instead of an fd. When used
 * in a PollerInterest, it must be the only flag set and `fd` is ignored.
 * POLLER_OP_ADD and POLLER_OP_MOD both arm the timer (replacing a previously
 * armed one) to expire after `timeout_ms`, and POLLER_OP_DEL disarms it.
 * When poller_event_flags returns it, the timer has expired and
 * poller_event_fd returns -1.
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

/*
 * An event returned by poller_wait. It is the native event struct of the
 * underlying API so that nothing is copied out of the kernel's result
 * array. Use poller_event_fd and poller_event_flags to inspect it.
 */
#ifdef POLLER_USE_EPOLL
typedef struct epoll_event PollerEvent;
#else
typedef struct kevent PollerEvent;
#endif

/*
 * Describes interest in one fd (or the timer) for poller_ctl.
 */
typedef struct PollerInterest {
  // The fd to register interest in. Always set, except for the timer where
  // it is ignored.
  int fd;

  // Events currently registered with the kernel, a combination of
  // POLLER_READ and POLLER_WRITE. Needed for POLLER_OP_MOD and
  // POLLER_OP_DEL, ignored for POLLER_OP_ADD. Only kqueue needs it because
  // it has a separate filter for read and write events, so to modify an fd
  // we must know which filters to remove. epoll replaces all events for an
  // fd in a single call. For the timer, set it to POLLER_TIMER to disarm.
  uint32_t old_events;

  // Events to register. Needed for POLLER_OP_ADD and POLLER_OP_MOD,
  // ignored for POLLER_OP_DEL. For the timer, set it to POLLER_TIMER.
  uint32_t new_events;

  // Only used when arming the timer (POLLER_TIMER in new_events):
  // milliseconds until it expires. Zero means as soon as possible.
  int timeout_ms;
} PollerInterest;

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
 * Adds, modifies or deletes interest in an fd (or the timer) in the poller.
 * See PollerInterest for which members are needed for each op.
 *
 * Returns zero on success and -1 on error. errno is set on error.
 */
int poller_ctl(Poller *poller, PollerOp op, const PollerInterest *interest);

/*
 * Fills the caller-allocated `out` array of `max_events` items with events indicating potentially
 * readable or writable fds. If it is -1 it blocks until an event is available. If it zero or
 * positive, it waits for a maximum of that many milliseconds before returning. Zero makes it return
 * immediately
 *
 * On success returns the number of items placed in the `out` array.
 * On error returns -1. errno is set on error.
 */
int poller_wait(Poller *poller, PollerEvent *out, int max_events, int timeout_ms);

/*
 * Returns the fd of an event returned by poller_wait, or -1 for the timer.
 */
int poller_event_fd(const Poller *poller, const PollerEvent *ev);

/*
 * Returns the POLLER_READ and POLLER_WRITE flags of an event returned by
 * poller_wait, or POLLER_TIMER if the timer expired.
 */
uint32_t poller_event_flags(const Poller *poller, const PollerEvent *ev);

#endif /* PG_HTTP_POLLER_H */
