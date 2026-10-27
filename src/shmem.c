#include "shmem.h"
#include "pg_prelude.h"

/*
 * Initialize worker shared state
 */
void init_worker_shared_state(WorkerSharedState *state) {
  state->procLatch = NULL;
  pg_atomic_init_u32(&state->got_restart, 0);
  pg_atomic_init_u32(&state->status, WS_NOT_YET);
  ConditionVariableInit(&state->cv);
}

void wake_worker(void) {
  if (worker_shared_state == NULL) return;

  // Load once, the worker clears the pointer when it exits
  Latch *latch = worker_shared_state->procLatch;
  if (latch != NULL) SetLatch(latch);
}

static shmem_request_hook_type prev_shmem_request_hook = NULL;

/*
 * Hook to request shared memory required by the background worker.
 * This runs in the postmaster process.
 */
static void shmem_request(void) {
  if (prev_shmem_request_hook) prev_shmem_request_hook();

  RequestAddinShmemSpace(MAXALIGN(sizeof(WorkerSharedState)));
}

static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
WorkerSharedState             *worker_shared_state     = NULL;

/*
 * Hook to initialize shared memory. This runs in the postmaster process.
 */
static void shmem_init(void) {
  if (prev_shmem_startup_hook) prev_shmem_startup_hook();

  bool found;

  LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

  worker_shared_state =
      ShmemInitStruct(EXTNAME " worker shared state", sizeof(WorkerSharedState), &found);

  if (!found) {
    init_worker_shared_state(worker_shared_state);
  }

  LWLockRelease(AddinShmemInitLock);
}

/*
 * Register shared memory hooks
 */
void register_shmem_hooks(void) {
  prev_shmem_request_hook = shmem_request_hook;
  shmem_request_hook      = shmem_request;

  prev_shmem_startup_hook = shmem_startup_hook;
  shmem_startup_hook      = shmem_init;
}
