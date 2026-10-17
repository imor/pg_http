#ifndef PG_HTTP_POLLER_H
#define PG_HTTP_POLLER_H

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
 * The main poller struct containing the root file descriptor for the
 * multiplexing API (epoll or kqueue). This must be initialized with a
 * call to `poller_create` and when no longer needed destroyed with
 * a call to `poller_close`.
 */
typedef struct Poller {
  // The root fd created by a call to `epoll_create1` or `kqueue`.
  int fd;

#ifdef POLLER_USE_EPOLL
  // Timer fd created by a call to `timerfd_create` to implement a
  // oneshot timer. This is only set the first time a caller calls
  // `poller_ctl` with a timer event. Until then its value is -1.
  // Only needed for epoll since kqueue has support for timer
  // filters and it doesn't need  a separate fd for a timer.
  int timer_fd;
#endif
} Poller;

/*
 * The operation to perform when calling `poller_ctl`.
 */
typedef enum PollerOp {
  // Add the fd to the interest list.
  POLLER_OP_ADD,

  // Modifiy the fd in the interest list.
  POLLER_OP_MOD,

  // Delete the fd from the interest list.
  POLLER_OP_DEL
} PollerOp;

/*
 * The interest to register when calling `poller_ctl`.
 */
typedef struct PollerInterest {
  // The fd to register interest in. Set for `POLLER_EV_READ` and
  // `POLLER_EV_WRITE` events. Ignored for `POLLER_EV_TIMER`.
  int fd;

  // A combination of `PollerEventType` enums bitflags to register.
  // Required for `POLLER_OP_ADD` and `POLLER_OP_MOD`, ignored for
  // `POLLER_OP_DEL`.
  uint32_t new_events;

  // Events previously registered with the kernel. Required for `POLLER_OP_MOD`
  // and `POLLER_OP_DEL`. Ignored for `POLLER_OP_ADD`. Used only by the kqueue
  // API because it has a separate filter for read and write events. So to
  // modify or delete existing filters, those must be explicitly passed in
  // to `poller_ctl`. epoll keeps a single combined filter which it replaces
  // when `epoll_ctl` is called, so doesn't need to know the old registered
  // events.
  uint32_t old_events;

  // Number of milliseconds to set the timer to. Zero means as soon
  // as possible. Used when `new_events` contains `POLLER_EV_TIMER`.
  // Ignored otherwise.
  int timeout_ms;
} PollerInterest;

/*
 * An array of these events is returned by `poller_wait` indicating the kinds
 * of events that actually are ready to be acted upon. It is a typedef to the
 * underlying I/O multiplexing API's event type to avoid copying data between
 * the native and another type if we were to create such a struct.
 */
#ifdef POLLER_USE_EPOLL
typedef struct epoll_event PollerEvent;
#else
typedef struct kevent PollerEvent;
#endif

/*
 * Poller event types. It is used in `poller_ctl` when registering interest for
 * an fd. `poller_event_kind` can be used to convert from the native event type
 * to it.
 *
 * It is a flags enum and its members can be bitwise combined in a single
 * variable to represent multiple events.
 */
typedef enum {
  // Represents no events. Useful to initialize a variable to
  // an empty set of events.
  POLLER_EV_NONE = 0,

  // Represents a read event.
  POLLER_EV_READ = 1 << 0,

  // Represents a write event.
  POLLER_EV_WRITE = 1 << 1,

  // Represents both read and write events.
  POLLER_EV_BOTH = POLLER_EV_READ | POLLER_EV_WRITE,

  // Represents a timer event.
  POLLER_EV_TIMER = 1 << 2
} PollerEventType;

/*
 * Initializes a new poller instance by setting its fd to a file descriptor
 * created by the I/O multiplexing API. Returns zero on success and -1 on
 * error. `errno` is set on error.
 */
int poller_create(Poller *poller);

/*
 * Adds, modifies or deletes interest in an fd, or the timer, in the poller.
 * See `PollerInterest` for which members are needed for each op.
 *
 * Returns zero on success and -1 on error. errno is set on error.
 */
int poller_ctl(Poller *poller, PollerOp op, const PollerInterest *interest);

/*
 * Fills the caller-allocated `out` array of `max_events` items with events
 * indicating potentially readable or writable fds or a timer event.
 *
 * If `timeout_ms` is -1 it blocks until an event is available. If it zero or
 * positive, it waits for a maximum of that many milliseconds before returning.
 * Zero makes it return immediately without blocking.
 *
 * On success returns the number of items placed in the `out` array.
 * On error returns -1. errno is set on error.
 */
int poller_wait(Poller *poller, PollerEvent *out, int max_events, int timeout_ms);

/*
 * Closes a poller instance. Returns zero on success and -1 on error.
 * errno is set on error.
 */
int poller_close(Poller *poller);

/*
 * Returns the fd of an event returned by poller_wait, or -1 for the timer.
 */
int poller_event_fd(const Poller *poller, const PollerEvent *ev);

/*
 * Converts the native event kind to the `PollerEventKind` enum member.
 */
uint32_t poller_event_kind(const Poller *poller, const PollerEvent *ev);

#endif /* PG_HTTP_POLLER_H */
