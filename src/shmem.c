#include "shmem.h"
#include "pg_prelude.h"

/*
 * Initialize worker shared state
 */
void init_worker_shared_state(WorkerSharedState *state) {
  for (int i = 0; i < MAX_WORKERS; i++) {
    state->workers[i].procLatch = NULL;
    pg_atomic_init_u32(&state->workers[i].got_restart, 0);
    pg_atomic_init_u32(&state->workers[i].status, WS_NOT_YET);
  }
  ConditionVariableInit(&state->cv);
}

void wake_workers(void) {
  if (worker_shared_state == NULL) return;

  for (int i = 0; i < guc_workers; i++) {
    // Load once, the worker clears the pointer when it exits
    Latch *latch = worker_shared_state->workers[i].procLatch;
    if (latch != NULL) SetLatch(latch);
  }
}

void restart_workers(void) {
  for (int i = 0; i < guc_workers; i++)
    pg_atomic_write_u32(&worker_shared_state->workers[i].got_restart, 1);
  wake_workers();
}

bool all_workers_running(void) {
  for (int i = 0; i < guc_workers; i++) {
    if (pg_atomic_read_u32(&worker_shared_state->workers[i].status) != WS_RUNNING) return false;
  }
  return true;
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
