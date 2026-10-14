#ifndef PG_HTTP_SHMEM_H
#define PG_HTTP_SHMEM_H

#include "pg_prelude.h"

/*
 * Background worker state saved in the shared memory.
 * Used to communicate with other backends.
 */
typedef struct
{
    // Background worker process's latch. Same as MyLatch global in a process.
    // Added to shared memory so that other backends can send signals to the
    // pg_http background worker.
    Latch *procLatch;
} WorkerSharedState;

// Background worker state in shared memory, NULL until shmem_init runs
extern WorkerSharedState *worker_shared_state;

/*
 * Register shared memory hooks
 */
void register_shmem_hooks(void);

#endif /* PG_HTTP_SHMEM_H */
