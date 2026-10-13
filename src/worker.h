#ifndef WORKER_H
#define WORKER_H

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

PGDLLEXPORT void pg_http_main(Datum main_arg);

#endif /* WORKER_H */
