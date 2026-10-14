#include "worker.h"
#include "guc.h"
#include "signals.h"

static uint32 main_wait_event = 0;

/*
 * The main entry point for the pg_http background worker.
 */
void pg_http_main(__attribute__((unused)) Datum main_arg) {
  ereport(LOG, errmsg("inside pg_http_main function"));

  // Setup signals
  pqsignal(SIGTERM, handle_sigterm);
  BackgroundWorkerUnblockSignals();

  // Connect to the database
  BackgroundWorkerInitializeConnection(guc_database_name, guc_username, 0);

  // Set application_name column in pg_stat_activity view
  pgstat_report_appname("pg_http");

  // Set state, state_change, query, and query_start fields in pg_stat_activity view
  pgstat_report_activity(STATE_RUNNING, "pg_http: initializing");

  if (main_wait_event == 0) main_wait_event = WaitEventExtensionNew("PgHttpMain");

  // Start main loop
  while (true) {
    pgstat_report_activity(STATE_IDLE, "");
    (void)WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 1000,
                    main_wait_event);
    ResetLatch(MyLatch);
    pgstat_report_activity(STATE_RUNNING, "pg_http: working");

    CHECK_FOR_INTERRUPTS();

    if (termination_requested) {
      pgstat_report_activity(STATE_RUNNING, "pg_http: exiting");
      break;
    }
  }

  ereport(LOG, errmsg("exiting pg_http_main function"));

  // We return with and exit code of 1 to ensure that Postgres
  // restarts the background worker
  proc_exit(1);
}
