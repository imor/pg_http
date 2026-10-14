#ifndef PG_SIGNALS_H
#define PG_SIGNALS_H

#include "pg_prelude.h"

#include <signal.h>

extern volatile sig_atomic_t termination_requested;
extern void                  handle_sigterm(SIGNAL_ARGS);
extern void                  setup_signals(void);

#endif /* PG_SIGNALS_H */
