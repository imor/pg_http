#ifndef PG_HTTP_SHMEM_H
#define PG_HTTP_SHMEM_H

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
typedef struct WorkerSharedState {
  // Background worker process's latch. Same as MyLatch global in a process.
  // Added to shared memory so that other backends can send signals to the
  // pg_http background worker.
  // NULL while the worker is not running. Read it once into a local before using it.
  Latch *volatile procLatch;

  // Set by `http.worker_restart()` to ask the worker to exit so that it gets restarted
  pg_atomic_uint32 got_restart;

  // A `WorkerStatus`, published by the worker
  pg_atomic_uint32 status;

  // Broadcast whenever `status` changes
  ConditionVariable cv;
} WorkerSharedState;

/*
 * Initialize worker shared state
 */
void init_worker_shared_state(WorkerSharedState *state);

// Background worker state in shared memory, NULL until shmem_init runs
extern WorkerSharedState *worker_shared_state;

/*
 * Sets the worker's latch, if the worker is running
 */
void wake_worker(void);

/*
 * Register shared memory hooks
 */
void register_shmem_hooks(void);

#endif /* PG_HTTP_SHMEM_H */
