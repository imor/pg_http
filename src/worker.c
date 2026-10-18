#include "worker.h"
#include "guc.h"
#include "http/client.h"
#include "signals.h"

#include <errno.h>
#include <string.h>

/*
 * The URL requested on startup, hardcoded for testing purposes.
 */
#define TEST_URL "http://example.org/"

/*
 * Goes to sleep on the main wait event set waiting for the following events:
 *
 * * Signal handlers (via the latch)
 * * Timers and network (via the poller's root fd)
 *
 * Returns true if termination has been requested, false otherwise.
 */
static bool wait_for_events(WaitEventSet *set, uint32 main_wait_event) {
  WaitEvent event;

  pgstat_report_activity(STATE_IDLE, "");

  int n = WaitEventSetWait(set, -1, &event, 1, main_wait_event);
  if (n > 0 && (event.events & WL_LATCH_SET)) {
    ResetLatch(MyLatch);
  }

  CHECK_FOR_INTERRUPTS();

  if (termination_requested) {
    pgstat_report_activity(STATE_RUNNING, "pg_http: exiting");
    return true;
  }

  if (ConfigReloadPending) {
    ConfigReloadPending = false;
    ProcessConfigFile(PGC_SIGHUP);
  }

  return false;
}

/*
 * The main loop of the background worker.
 */
static int main_loop(uint32 main_wait_event) {
  HttpClient client;

  if (http_client_init(&client) < 0) {
    ereport(ERROR, errmsg("pg_http: could not initialize http client"));
  }

  WaitEventSet *set = CreateWaitEventSet(NULL, 3);
  AddWaitEventToSet(set, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
  AddWaitEventToSet(set, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET, NULL, NULL);
  AddWaitEventToSet(set, WL_SOCKET_READABLE, http_client_fd(&client), NULL, NULL);

  HttpRequest req = {.method = HTTP_GET, .url = TEST_URL};
  if (http_client_send(&client, &req) < 0) {
    ereport(WARNING, errmsg("pg_http: could not start request to %s", TEST_URL));
  }

  while (true) {
    if (wait_for_events(set, main_wait_event)) {
      break;
    }

    // The poller's root fd is level triggered, so it is safe to only look
    // at it with a zero timeout here. This never blocks.
    pgstat_report_activity(STATE_RUNNING, "pg_http: working");
    http_client_process(&client);

    const HttpResponse *res;
    while ((res = http_client_next(&client)) != NULL) {
      if (res->curl_code != 0) {
        ereport(LOG, errmsg("pg_http: request failed: %s", res->error));
      } else {
        ereport(LOG, errmsg("pg_http: request finished with status %d, %d headers, %d body bytes",
                            res->status, (int)res->header_count, (int)res->body.len));
      }
    }
  }

  FreeWaitEventSet(set);
  http_client_close(&client);

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
