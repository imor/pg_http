#include "worker.h"
#include "guc.h"
#include "signals.h"

/*
 * The main loop of the background worker.
 * It waits for events coming from:
 *
 * * Signal handlers
 * * Timers
 * * Network
 *
 * And takes appropriate action based on that. Else it sleeps
 * to avoid burning CPU cycles.
 */
static int main_loop(uint32 main_wait_event) {
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

    if (ConfigReloadPending) {
      ConfigReloadPending = false;
      ProcessConfigFile(PGC_SIGHUP);
    }
  }

  // We return with an exit code of 1 to ensure that Postgres
  // restarts the background worker
  return 1;
}

/*
 * The main entry point for the pg_http background worker.
 */
void pg_http_main(__attribute__((unused)) Datum main_arg) {
  ereport(LOG, errmsg("inside pg_http_main function"));

  setup_signals();

  // Connect to the database
  BackgroundWorkerInitializeConnection(guc_database_name, guc_username, 0);

  // Set application_name column in pg_stat_activity view
  pgstat_report_appname("pg_http");

  // Set state, state_change, query, and query_start fields in pg_stat_activity view
  pgstat_report_activity(STATE_RUNNING, "pg_http: initializing");

  uint32 main_wait_event = WaitEventExtensionNew("PgHttpMain");

  int code = main_loop(main_wait_event);

  ereport(LOG, errmsg("exiting pg_http_main function"));

  proc_exit(code);
}
