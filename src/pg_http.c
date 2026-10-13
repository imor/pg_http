#include "pg_prelude.h"

#include <curl/curl.h>
#include <stdbool.h>
#include <string.h>

#include "worker.h"

PG_MODULE_MAGIC;

static shmem_request_hook_type prev_shmem_request_hook = NULL;

// Hook to request shared memory required by the background worker
static void shmem_request(void)
{
  if (prev_shmem_request_hook)
    prev_shmem_request_hook();

  RequestAddinShmemSpace(MAXALIGN(sizeof(WorkerSharedState)));
}

static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static WorkerSharedState *worker_shared_state;

// Hook to initialize shared memory
static void shmem_init(void)
{
  if (prev_shmem_startup_hook)
    prev_shmem_startup_hook();

  bool found;

  LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

  worker_shared_state = ShmemInitStruct("pg_http worker shared state", sizeof(WorkerSharedState), &found);

  if (!found)
  {
    worker_shared_state->procLatch = NULL;
  }

  LWLockRelease(AddinShmemInitLock);
}

/*
 * The function called by postmaster during initialization.
 *
 * This function should:
 *
 * * Register any GUCs.
 * * Register pg_http's background worker.
 * * Register shared memory hooks
 *
 */
void _PG_init(void)
{
  // If it is a binary upgrade we return early because running a background
  // worker in binary upgrade mode might interfere with the upgrade.
  if (IsBinaryUpgrade)
  {
    return;
  }

  // Ensure that pg_http is configured in shared_preload_libraries
  if (!process_shared_preload_libraries_in_progress)
  {
    ereport(ERROR, errmsg("pg_http is not in shared_preload_libraries"),
            errhint("Add pg_http to the shared_preload_libraries GUC in postgresql.conf."));
  }

  // Register background worker to run after recovery finishes with
  // support for connecting to the database and shared memory. If the worker
  // crashes or exits, it should restart after one second.
  RegisterBackgroundWorker(&(BackgroundWorker){
      .bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION,
      .bgw_start_time = BgWorkerStart_RecoveryFinished,
      .bgw_library_name = "pg_http",
      .bgw_function_name = "pg_http_main",
      .bgw_name = "pg_http " EXTVERSION " worker",
      .bgw_type = "pg_http worker",
      .bgw_restart_time = 1,
  });

  // Register shared memory hooks
  prev_shmem_request_hook = shmem_request_hook;
  shmem_request_hook = shmem_request;

  prev_shmem_startup_hook = shmem_startup_hook;
  shmem_startup_hook = shmem_init;
}
