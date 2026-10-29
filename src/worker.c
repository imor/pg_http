#include "worker.h"
#include "exchange.h"
#include "guc.h"
#include "http/client.h"
#include "pending.h"
#include "schedule.h"
#include "shmem.h"
#include "signals.h"

/*
 * Called for every request which has completed. Its handle was removed from the multi handle.
 */
static void on_response(CURLcode code, HttpTransfer *transfer, long status_code) {
  pending_results_add(http_transfer_finish(transfer, code, status_code));
}

/*
 * Drives the transfers in flight and collects the completed ones as pending results.
 */
static void process_transfers(HttpClient *client) {
  if (http_client_process(client) != HTTP_CLIENT_OK ||
      http_client_get_transfers(client, on_response) != HTTP_CLIENT_OK) {
    ereport(WARNING, errmsg(EXTNAME ": error while processing http requests"));
  }
}

/*
 * Reports the worker as idle when there is nothing to do until something wakes it.
 */
static void report_activity(const Schedule *schedule, const HttpClient *client) {
  bool busy = schedule_has_work(schedule) || client->running_handles > 0;
  pgstat_report_activity(busy ? STATE_RUNNING : STATE_IDLE, busy ? EXTNAME ": working" : "");
}

static WaitEventSet *create_wait_event_set(HttpClient *client) {
  WaitEventSet *set = CreateWaitEventSet(NULL, 3);
  AddWaitEventToSet(set, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
  AddWaitEventToSet(set, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET, NULL, NULL);
  AddWaitEventToSet(set, WL_SOCKET_READABLE, http_client_fd(client), NULL, NULL);
  return set;
}

/*
 * Goes to sleep on the main wait event set waiting for the following events:
 *
 * * Signal handlers (via the latch)
 * * Timers and network (via the poller's root fd)
 *
 * `timeout_ms` is -1 to wait forever. Returns true if the worker must exit, either because
 * termination or a restart was requested. Sets `woken` if the latch was set, i.e. a wake was
 * requested.
 */
static bool wait_for_events(WaitEventSet *set, uint32 main_wait_event, long timeout_ms,
                            bool *woken) {
  WaitEvent event;

  int n  = WaitEventSetWait(set, timeout_ms, &event, 1, main_wait_event);
  *woken = n > 0 && (event.events & WL_LATCH_SET);
  if (*woken) {
    ResetLatch(MyLatch);
  }

  CHECK_FOR_INTERRUPTS();

  if (termination_requested) {
    pgstat_report_activity(STATE_RUNNING, EXTNAME ": exiting");
    return true;
  }

  if (pg_atomic_exchange_u32(&worker_shared_state->got_restart, 0)) {
    pgstat_report_activity(STATE_RUNNING, EXTNAME ": restarting");
    return true;
  }

  if (ConfigReloadPending) {
    ConfigReloadPending = false;
    ProcessConfigFile(PGC_SIGHUP);
  }

  return false;
}

static void publish_status(WorkerStatus status) {
  pg_atomic_write_u32(&worker_shared_state->status, (uint32)status);
  ConditionVariableBroadcast(&worker_shared_state->cv);
}

static void on_worker_exit(__attribute__((unused)) int code, __attribute__((unused)) Datum arg) {
  worker_shared_state->procLatch = NULL;
  publish_status(WS_EXITED);
}

/*
 * The main loop of the background worker.
 */
static int main_loop(uint32 main_wait_event) {
  HttpClient client;

  if (http_client_init(&client) != HTTP_CLIENT_OK) {
    ereport(ERROR, errmsg(EXTNAME ": could not initialize http client"));
  }

  WaitEventSet *set = create_wait_event_set(&client);

  publish_status(WS_RUNNING);

  Schedule schedule = schedule_init();

  while (true) {
    report_activity(&schedule, &client);

    bool woken;
    if (wait_for_events(set, main_wait_event, schedule_timeout_ms(&schedule), &woken)) {
      break;
    }

    pgstat_report_activity(STATE_RUNNING, EXTNAME ": working");

    process_transfers(&client);

    ExchangePlan plan = schedule_plan(&schedule, client.running_handles, woken);
    if (plan.retire || plan.capacity > 0) {
      ExchangeOutcome outcome = exchange_with_queue(&client, plan);
      schedule_after_exchange(&schedule, plan, outcome);
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
  ereport(LOG, errmsg("inside " EXTNAME "_main function"));

  worker_shared_state->procLatch = MyLatch;
  on_proc_exit(on_worker_exit, 0);

  setup_signals();

  // Connect to the database
  BackgroundWorkerInitializeConnection(guc_database_name, guc_username, 0);

  // Set application_name column in pg_stat_activity view
  pgstat_report_appname(EXTNAME);

  // Set state, state_change, query, and query_start fields in pg_stat_activity view
  pgstat_report_activity(STATE_RUNNING, EXTNAME ": initializing");

  uint32 main_wait_event = WaitEventExtensionNew("PgHttpMain");

  int code = main_loop(main_wait_event);

  ereport(LOG, errmsg("exiting " EXTNAME "_main function"));

  proc_exit(code);
}
