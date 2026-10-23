#include "worker.h"
#include "guc.h"
#include "http/client.h"
#include "queue/table.h"
#include "signals.h"

#include <errno.h>
#include <string.h>

/*
 * The maximum number of requests read from the queue table in one batch.
 */
#define BATCH_SIZE 100

/*
 * Runs `fn` inside a transaction with an SPI connection and an active snapshot.
 */
static void run_in_transaction(void (*fn)(void *), void *arg) {
  SetCurrentStatementStartTimestamp();
  StartTransactionCommand();
  PushActiveSnapshot(GetTransactionSnapshot());

  int ret_code = SPI_connect();
  if (ret_code != SPI_OK_CONNECT)
    ereport(ERROR, errmsg(EXTNAME ": SPI_connect failed: %s", SPI_result_code_string(ret_code)));

  fn(arg);

  ret_code = SPI_finish();
  if (ret_code != SPI_OK_FINISH)
    ereport(ERROR, errmsg(EXTNAME ": SPI_finish failed: %s", SPI_result_code_string(ret_code)));

  PopActiveSnapshot();
  CommitTransactionCommand();
}

static void do_retire(void *arg) {
  if (retire((HttpResponse *)arg, 1) != HTTP_TABLE_QUEUE_OK) {
    ereport(ERROR, errmsg(EXTNAME ": could not retire request"));
  }
}

static void do_reset_in_flight(__attribute__((unused)) void *arg) {
  if (reset_in_flight() != HTTP_TABLE_QUEUE_OK) {
    ereport(ERROR, errmsg(EXTNAME ": could not reset in flight requests"));
  }
}

/*
 * Called for every request which has completed.
 */
static void on_response(CURLcode code, HttpResponse *response, long status_code) {
  if (code != CURLE_OK) {
    ereport(WARNING, errmsg(EXTNAME ": request failed: %s", curl_easy_strerror(code)));
  } else {
    ereport(LOG, errmsg(EXTNAME ": got response with status %ld and %d body bytes", status_code,
                        response->body.len));

    // The request has been answered so it is removed from the queue
    run_in_transaction(do_retire, response);
  }

  // Responses are allocated in send_batch and owned by us once completed
  http_response_close(response);
  pfree(response);
}

/*
 * Reads a batch of requests from the queue table and starts sending them.
 *
 * The transaction is only held open while reading from the table. The
 * requests are allocated in a separate memory context so they outlive it.
 */
static void send_batch(HttpClient *client) {
  // The Internal variant avoids the GNU statement expression in the macro
  MemoryContext batch_context =
      AllocSetContextCreateInternal(TopMemoryContext, EXTNAME " batch", ALLOCSET_DEFAULT_MINSIZE,
                                    ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE);

  HttpRequest *requests     = NULL;
  int          num_requests = 0;

  SetCurrentStatementStartTimestamp();
  StartTransactionCommand();
  PushActiveSnapshot(GetTransactionSnapshot());

  // SPI_palloc allocates in the context current at SPI_connect, so connect
  // while in the batch context to make the requests survive the commit.
  MemoryContext old_context = MemoryContextSwitchTo(batch_context);

  int ret_code = SPI_connect();
  if (ret_code != SPI_OK_CONNECT)
    ereport(ERROR, errmsg(EXTNAME ": SPI_connect failed: %s", SPI_result_code_string(ret_code)));

  if (dequeue(BATCH_SIZE, &requests, &num_requests) != HTTP_TABLE_QUEUE_OK) {
    ereport(ERROR, errmsg(EXTNAME ": could not read requests from the queue"));
  }

  ret_code = SPI_finish();
  if (ret_code != SPI_OK_FINISH)
    ereport(ERROR, errmsg(EXTNAME ": SPI_finish failed: %s", SPI_result_code_string(ret_code)));

  MemoryContextSwitchTo(old_context);

  PopActiveSnapshot();
  CommitTransactionCommand();

  for (int i = 0; i < num_requests; i++) {
    // Responses outlive this function so they go in a long lived context
    MemoryContext      old      = MemoryContextSwitchTo(TopMemoryContext);
    HttpResponse      *response = palloc(sizeof(HttpResponse));
    HttpResponseStatus status   = http_response_init(response);
    response->request_id        = requests[i].id;
    MemoryContextSwitchTo(old);

    if (status != HTTP_RESPONSE_OK) {
      ereport(ERROR, errmsg(EXTNAME ": could not initialize http response"));
    }

    // curl copies the url, so requests can be freed after this loop
    if (http_client_send(client, &requests[i], response) != HTTP_CLIENT_OK) {
      ereport(WARNING, errmsg(EXTNAME ": could not start request to %s", requests[i].url));
      http_response_close(response);
      pfree(response);
    }
  }

  MemoryContextDelete(batch_context);
}

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
    pgstat_report_activity(STATE_RUNNING, EXTNAME ": exiting");
    return true;
  }

  if (ConfigReloadPending) {
    ConfigReloadPending = false;
    ProcessConfigFile(PGC_SIGHUP);
  }

  return false;
}

/*
 * Returns true if this extension has been created, false otherwise
 */
static bool extension_created(void) {
  SetCurrentStatementStartTimestamp();
  StartTransactionCommand();

  Oid  extension_oid     = get_extension_oid(EXTNAME, true);
  bool extension_present = extension_oid != InvalidOid;

  bool extension_script_executed = !(creating_extension && CurrentExtensionObject == extension_oid);

  CommitTransactionCommand();

  return extension_present && extension_script_executed && !IsBinaryUpgrade;
}

/*
 * The main loop of the background worker.
 */
static int main_loop(uint32 main_wait_event) {
  HttpClient client;

  if (http_client_init(&client) != HTTP_CLIENT_OK) {
    ereport(ERROR, errmsg(EXTNAME ": could not initialize http client"));
  }

  WaitEventSet *set = CreateWaitEventSet(NULL, 3);
  AddWaitEventToSet(set, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
  AddWaitEventToSet(set, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET, NULL, NULL);
  AddWaitEventToSet(set, WL_SOCKET_READABLE, http_client_fd(&client), NULL, NULL);

  bool in_flight_reset = false;

  while (true) {
    if (wait_for_events(set, main_wait_event)) {
      break;
    }

    pgstat_report_activity(STATE_RUNNING, EXTNAME ": working");

    if (!extension_created()) {
      continue;
    }

    // Requests which were in flight when the worker last stopped are retried
    if (!in_flight_reset) {
      run_in_transaction(do_reset_in_flight, NULL);
      in_flight_reset = true;
    }

    send_batch(&client);

    if (http_client_process(&client) != HTTP_CLIENT_OK ||
        http_client_get_responses(&client, on_response) != HTTP_CLIENT_OK) {
      ereport(WARNING, errmsg(EXTNAME ": error while processing http requests"));
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
