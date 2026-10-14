#include "signals.h"

volatile sig_atomic_t termination_requested = 0;

/*
 * The SIGTERM signal handler.
 *
 * Sets a flag to indicate to the background worker
 * to exit gracefully. Calls SetLatch to wake the worker
 * if it was sleeping.
 */
void handle_sigterm(__attribute__((unused)) SIGNAL_ARGS) {
  int save_errno = errno;

  termination_requested = 1;
  SetLatch(MyLatch);

  errno = save_errno;
}
