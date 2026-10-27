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
    ereport(ERROR, errmsg(EXTNAME " is not in shared_preload_libraries"),
            errhint("Add " EXTNAME " to the shared_preload_libraries GUC in postgresql.conf."));
  }

  // The number of workers is a GUC, so it has to be defined first
  register_gucs();

  // The logical replication launcher takes a slot too. A worker which doesn't get one never
  // starts, and the requests of its share would never be processed.
  if (guc_workers >= max_worker_processes) {
    ereport(ERROR, errmsg(EXTNAME ".workers (%d) needs a larger max_worker_processes (%d)",
                          guc_workers, max_worker_processes),
            errhint("Set max_worker_processes higher than " EXTNAME ".workers."));
  }

  // Register the background workers to run after recovery finishes with
  // support for connecting to the database and shared memory. If a worker
  // crashes or exits, it should restart after one second.
  for (int i = 0; i < guc_workers; i++) {
    BackgroundWorker worker = {
      .bgw_flags         = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION,
      .bgw_start_time    = BgWorkerStart_RecoveryFinished,
      .bgw_library_name  = EXTNAME,
      .bgw_function_name = EXTNAME "_main",
      .bgw_type          = EXTNAME " worker",
      .bgw_restart_time  = 1,
      .bgw_main_arg      = Int32GetDatum(i),
    };
    snprintf(worker.bgw_name, BGW_MAXLEN, EXTNAME " " EXTVERSION " worker");
    if (i > 0) snprintf(worker.bgw_name, BGW_MAXLEN, EXTNAME " " EXTVERSION " worker %d", i);
    RegisterBackgroundWorker(&worker);
  }

  register_shmem_hooks();
}
