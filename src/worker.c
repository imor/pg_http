#include "worker.h"
#include "guc.h"

/*
 * The main entry point for the pg_http background worker.
 */
void pg_http_main(__attribute__((unused)) Datum main_arg)
{
    ereport(LOG, errmsg("inside pg_http_main function"));

    // Setup signals
    pqsignal(SIGTERM, die);
    BackgroundWorkerUnblockSignals();

    // Connect to the database
    BackgroundWorkerInitializeConnection(guc_database_name, guc_username, 0);

    // Set application_name column in pg_stat_activity view
    pgstat_report_appname("pg_http");

    // Start main loop
    while (true)
    {
        (void)WaitLatch(MyLatch,
                        WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                        1000, PG_WAIT_EXTENSION);
        ResetLatch(MyLatch);

        CHECK_FOR_INTERRUPTS();
    }

    ereport(LOG, errmsg("exiting pg_http_main function"));
}
