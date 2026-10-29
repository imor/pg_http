#include "shmem.h"
#include "pg_prelude.h"

/*
 * Initialize worker shared state
 */
void init_worker_shared_state(WorkerSharedState *state) {
  state->procLatch = NULL;
  pg_atomic_init_u32(&state->wake_requested, 0);
  pg_atomic_init_u32(&state->got_restart, 0);
  pg_atomic_init_u32(&state->status, WS_NOT_YET);
  ConditionVariableInit(&state->cv);
}

void wake_worker(void) {
  if (worker_shared_state == NULL) return;

  // Load once, the worker clears the pointer when it exits
  Latch *latch = worker_shared_state->procLatch;
  if (latch != NULL) {
    pg_atomic_write_u32(&worker_shared_state->wake_requested, 1);
    SetLatch(latch);
  }
}

#if PG_VERSION_NUM >= 150000
static shmem_request_hook_type prev_shmem_request_hook = NULL;
#endif

/*
 * Hook to request shared memory required by the background worker.
 * This runs in the postmaster process.
 */
static void shmem_request(void) {
#if PG_VERSION_NUM >= 150000
  if (prev_shmem_request_hook) prev_shmem_request_hook();
#endif

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
#if PG_VERSION_NUM >= 150000
  prev_shmem_request_hook = shmem_request_hook;
  shmem_request_hook      = shmem_request;
#else
  // Before PG 15 there is no request hook, shmem is requested directly from _PG_init
  shmem_request();
#endif

  prev_shmem_startup_hook = shmem_startup_hook;
  shmem_startup_hook      = shmem_init;
}
