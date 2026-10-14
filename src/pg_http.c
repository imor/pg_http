#include "guc.h"
#include "pg_prelude.h"
#include "shmem.h"
#include "worker.h"

PG_MODULE_MAGIC;

/*
 * The function called by postmaster during initialization.
 *
 * This function registers:
 *
 * * GUCs.
 * * pg_http's background worker.
 * * shared memory hooks
 *
 */
void _PG_init(void) {
  // If it is a binary upgrade we return early because running a background
  // worker in binary upgrade mode might interfere with the upgrade.
  if (IsBinaryUpgrade) {
    return;
  }

  // Ensure that pg_http is configured in shared_preload_libraries
  if (!process_shared_preload_libraries_in_progress) {
    ereport(ERROR, errmsg("pg_http is not in shared_preload_libraries"),
            errhint("Add pg_http to the shared_preload_libraries GUC in postgresql.conf."));
  }

  // Register background worker to run after recovery finishes with
  // support for connecting to the database and shared memory. If the worker
  // crashes or exits, it should restart after one second.
  RegisterBackgroundWorker(&(BackgroundWorker){
    .bgw_flags         = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION,
    .bgw_start_time    = BgWorkerStart_RecoveryFinished,
    .bgw_library_name  = "pg_http",
    .bgw_function_name = "pg_http_main",
    .bgw_name          = "pg_http " EXTVERSION " worker",
    .bgw_type          = "pg_http worker",
    .bgw_restart_time  = 1,
  });

  register_shmem_hooks();

  register_gucs();
}
