#ifndef PG_HTTP_SHMEM_H
#define PG_HTTP_SHMEM_H

#include "guc.h"
#include "pg_prelude.h"

#include <port/atomics.h>
#include <storage/condition_variable.h>

typedef enum {
  WS_NOT_YET = 0,
  WS_RUNNING = 1,
  WS_EXITED  = 2,
} WorkerStatus;

/*
 * Background worker state saved in the shared memory.
 * Used to communicate with other backends.
 */
typedef struct WorkerSlot {
  // Background worker process's latch. Same as MyLatch global in a process.
  // Added to shared memory so that other backends can send signals to the
  // pg_http background worker.
  // NULL while the worker is not running. Read it once into a local before using it.
  Latch *volatile procLatch;

  // Set by `http.worker_restart()` to ask the worker to exit so that it gets restarted
  pg_atomic_uint32 got_restart;

  // A `WorkerStatus`, published by the worker
  pg_atomic_uint32 status;
} WorkerSlot;

typedef struct WorkerSharedState {
  // One slot per worker, only the first `pg_http.workers` are used
  WorkerSlot workers[MAX_WORKERS];

  // Broadcast whenever the `status` of a worker changes
  ConditionVariable cv;
} WorkerSharedState;

/*
 * Initialize worker shared state
 */
void init_worker_shared_state(WorkerSharedState *state);

// Background worker state in shared memory, NULL until shmem_init runs
extern WorkerSharedState *worker_shared_state;

/*
 * Sets the latch of every worker which is running
 */
void wake_workers(void);

/*
 * Asks every worker to exit so that it gets restarted
 */
void restart_workers(void);

/*
 * Whether all the workers are running
 */
bool all_workers_running(void);

/*
 * Register shared memory hooks
 */
void register_shmem_hooks(void);

#endif /* PG_HTTP_SHMEM_H */
