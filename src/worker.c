#include "worker.h"
#include "guc.h"
#include "http/client.h"
#include "queue/table.h"
#include "shmem.h"
#include "signals.h"

#include <errno.h>
#include <string.h>
#include <utils/timestamp.h>

/*
 * The initial capacity of the array of results waiting to be retired
 */
#define PENDING_RESULTS_INITIAL_SIZE 256

/*
 * The time to wait before looking at the queue again after work was done, and before retrying
 * to retire results when the tables were locked by someone else
 */
#define RECHECK_INTERVAL_MS 1000

/*
 * Results waiting to be retired. The array grows when results can't be retired for a
 * while, e.g. while a user holds a lock on the tables.
 */
static HttpResult **pending_results      = NULL;
static int          num_pending_results  = 0;
static int          pending_results_size = 0;

// When the oldest pending result was added, only meaningful if there are pending results
static TimestampTz first_pending_time = 0;

/*
 * Whether the worker still has to reset the requests marked as in flight by a previous
 * worker instance.
 */
static bool reset_pending = true;

typedef void (*txn_fn)(void *);

/*
 * Runs `fn` inside a transaction with an SPI connection and an active snapshot.
 *
 * SPI_palloc allocates in the context current at SPI_connect. If `spi_context`
 * is not NULL the connection is made in it, so that memory allocated by SPI
 * (e.g. results copied out by `fn`) survives the commit.
 */
static void run_in_transaction(txn_fn fn, void *arg, MemoryContext spi_context) {
  SetCurrentStatementStartTimestamp();
  StartTransactionCommand();
  PushActiveSnapshot(GetTransactionSnapshot());

  MemoryContext old_context = CurrentMemoryContext;
  if (spi_context != NULL) MemoryContextSwitchTo(spi_context);

  int ret_code = SPI_connect();
  if (ret_code != SPI_OK_CONNECT)
    ereport(ERROR, errmsg(EXTNAME ": SPI_connect failed: %s", SPI_result_code_string(ret_code)));

  fn(arg);

  ret_code = SPI_finish();
  if (ret_code != SPI_OK_FINISH)
    ereport(ERROR, errmsg(EXTNAME ": SPI_finish failed: %s", SPI_result_code_string(ret_code)));

  // SPI_finish returns to the context SPI_connect was called in
  MemoryContextSwitchTo(old_context);

  PopActiveSnapshot();
  CommitTransactionCommand();

  // Background workers must flush their pending pgstat counters themselves, otherwise the
  // counters of their writes never reach the shared stats and autovacuum doesn't see them.
  pgstat_report_stat(false);
}

/*
 * Checks that this extension has been created and, if so, locks its tables until the end of the
 * transaction, so that DROP EXTENSION waits for us. Must be called in a transaction.
 */
static ExtensionLockStatus begin_extension_work(Oid *requests_relid) {
  Oid  extension_oid     = get_extension_oid(EXTNAME, true);
  bool extension_present = extension_oid != InvalidOid;

  bool extension_script_executed = !(creating_extension && CurrentExtensionObject == extension_oid);

  if (!(extension_present && extension_script_executed && !IsBinaryUpgrade)) {
    return EXTENSION_ABSENT;
  }

  return lock_extension_tables(requests_relid);
}

/*
 * Adds a result to the ones waiting to be retired, which take ownership of it.
 */
static void add_pending_result(HttpResult *result) {
  if (num_pending_results == pending_results_size) {
    int new_size =
        pending_results_size == 0 ? PENDING_RESULTS_INITIAL_SIZE : pending_results_size * 2;
    MemoryContext old_context = MemoryContextSwitchTo(TopMemoryContext);
    pending_results = pending_results == NULL
                          ? palloc(sizeof(HttpResult *) * (size_t)new_size)
                          : repalloc(pending_results, sizeof(HttpResult *) * (size_t)new_size);
    MemoryContextSwitchTo(old_context);
    pending_results_size = new_size;
  }

  if (num_pending_results == 0) first_pending_time = GetCurrentTimestamp();

  pending_results[num_pending_results++] = result;
}

static void drop_pending_results(void) {
  for (int i = 0; i < num_pending_results; i++) {
    http_result_destroy(pending_results[i]);
  }
  num_pending_results = 0;
}

/*
 * What a worker cycle does in its transaction, and what it found.
 */
typedef struct {
  // Input: retire the pending results
  bool retire;

  // Input: the maximum number of requests to read from the queue, 0 to not read any
  int capacity;

  // Output
  HttpRequest        *requests;
  int                 num_requests;
  Oid                 requests_relid;
  uint64              num_expired;
  ExtensionLockStatus status;
} CycleArgs;

/*
 * Retires the pending results, deletes the expired responses and takes the next requests from the
 * queue, all in one transaction.
 */
static void cycle_txn(void *arg) {
  CycleArgs *args = (CycleArgs *)arg;

  args->status = begin_extension_work(&args->requests_relid);
  if (args->status != EXTENSION_LOCKED) return;

  if (args->retire) {
    // Only the results whose request was read from the current incarnation of the extension are
    // retired, the others belong to a dropped extension.
    HttpResult **current = palloc(sizeof(HttpResult *) * (size_t)num_pending_results);
    int          count   = 0;
    for (int i = 0; i < num_pending_results; i++) {
      if (pending_results[i]->requests_relid == args->requests_relid)
        current[count++] = pending_results[i];
    }

    if (count > 0 && retire(current, count) != HTTP_TABLE_QUEUE_OK) {
      ereport(ERROR, errmsg(EXTNAME ": could not retire requests"));
    }
  }

  if (args->capacity > 0) {
    // Retry the requests which were marked as in flight by a previous worker instance, this
    // guarantees at-least-once semantics.
    if (reset_pending) {
      if (reset_in_flight() != HTTP_TABLE_QUEUE_OK) {
        ereport(ERROR, errmsg(EXTNAME ": could not reset in flight requests"));
      }
      reset_pending = false;
    }

    if (delete_expired(guc_ttl, guc_batch_size, &args->num_expired) != HTTP_TABLE_QUEUE_OK) {
      ereport(ERROR, errmsg(EXTNAME ": could not delete expired responses"));
    }

    if (dequeue(args->capacity, &args->requests, &args->num_requests) != HTTP_TABLE_QUEUE_OK) {
      ereport(ERROR, errmsg(EXTNAME ": could not read requests from the queue"));
    }
  }
}

/*
 * Called for every request which has completed. Its handle was removed from the multi handle.
 */
static void on_response(CURLcode code, HttpTransfer *transfer, long status_code) {
  add_pending_result(http_transfer_finish(transfer, code, status_code));
}

/*
 * Starts sending the requests of a batch, rejected requests get an error response.
 */
static void send_requests(HttpClient *client, HttpRequest *requests, int num_requests,
                          Oid requests_relid) {
  for (int i = 0; i < num_requests; i++) {
    HttpRequest *request = &requests[i];

    // The result or transfer takes over the request and its context, only the array is freed
    // with the batch
    char *rejection = http_request_validate(request, guc_max_timeout_ms);
    if (rejection != NULL) {
      add_pending_result(http_result_rejected(request, requests_relid, rejection));
      pfree(rejection);
      continue;
    }

    HttpTransfer *transfer = http_transfer_create(request, requests_relid);
    if (transfer == NULL) {
      ereport(ERROR, errmsg(EXTNAME ": could not initialize http transfer"));
    }

    if (http_client_send(client, transfer) != HTTP_CLIENT_OK) {
      ereport(WARNING, errmsg(EXTNAME ": could not start request to %s", transfer->request.url));
      add_pending_result(http_transfer_abort(transfer, "could not start the request"));
    }
  }
}

/*
 * One cycle of the worker: in one transaction, retires the pending results and reads the next
 * requests from the queue table, then starts sending the requests.
 *
 * The transaction is only held open while reading from the table. The
 * requests are allocated in a separate memory context so they outlive it.
 *
 * Returns the status of the extension. If the tables were locked by someone else, nothing was
 * done and the pending results are kept.
 */
static ExtensionLockStatus run_cycle(HttpClient *client, bool retire, int capacity,
                                     int *num_requests, uint64 *num_expired) {
  // The Internal variant avoids the GNU statement expression in the macro
  MemoryContext batch_context =
      AllocSetContextCreateInternal(TopMemoryContext, EXTNAME " batch", ALLOCSET_DEFAULT_MINSIZE,
                                    ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE);

  CycleArgs args = {.retire         = retire,
                    .capacity       = capacity,
                    .requests       = NULL,
                    .num_requests   = 0,
                    .requests_relid = InvalidOid,
                    .num_expired    = 0,
                    .status         = EXTENSION_ABSENT};

  // Connect to SPI in the batch context to make the requests survive the commit
  run_in_transaction(cycle_txn, &args, batch_context);

  if (args.status != EXTENSION_BUSY) {
    if (retire) drop_pending_results();
    send_requests(client, args.requests, args.num_requests, args.requests_relid);
  }

  MemoryContextDelete(batch_context);

  *num_requests = args.num_requests;
  *num_expired  = args.num_expired;

  return args.status;
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

  WaitEventSet *set = CreateWaitEventSet(NULL, 3);
  AddWaitEventToSet(set, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
  AddWaitEventToSet(set, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET, NULL, NULL);
  AddWaitEventToSet(set, WL_SOCKET_READABLE, http_client_fd(&client), NULL, NULL);

  publish_status(WS_RUNNING);

  // The first cycle runs right away, requests could have been added while no worker was
  // running, and wakes sent then are lost.
  bool        work_scheduled = true;
  TimestampTz work_time      = GetCurrentTimestamp();

  // Set once a read from the queue found less requests than it could take, i.e. the queue is
  // empty. Retiring doesn't refill from the queue then, until a wake or the recheck.
  bool queue_exhausted = false;

  // Retiring is not attempted before this time, set when the tables were locked by someone else
  TimestampTz retire_not_before = 0;

  while (true) {
    int retire_batch_size =
        guc_retire_batch_size > 0 ? guc_retire_batch_size : Max(1, guc_batch_size / 2);
    TimestampTz retire_time = 0;
    if (num_pending_results > 0) {
      retire_time = TimestampTzPlusMilliseconds(first_pending_time, guc_retire_timeout_ms);
      if (retire_time < retire_not_before) retire_time = retire_not_before;
    }

    // Sleep until the next timed event, or forever if there is none
    long        timeout_ms = -1;
    TimestampTz now        = GetCurrentTimestamp();
    if (work_scheduled) timeout_ms = (long)TimestampDifferenceMilliseconds(now, work_time);
    if (num_pending_results > 0) {
      long until_retire = (long)TimestampDifferenceMilliseconds(now, retire_time);
      if (timeout_ms < 0 || until_retire < timeout_ms) timeout_ms = until_retire;
    }

    // The worker is idle when there is nothing to do until something wakes it
    bool busy = work_scheduled || client.running_handles > 0 || num_pending_results > 0;
    pgstat_report_activity(busy ? STATE_RUNNING : STATE_IDLE, busy ? EXTNAME ": working" : "");

    bool woken;
    if (wait_for_events(set, main_wait_event, timeout_ms, &woken)) {
      break;
    }

    pgstat_report_activity(STATE_RUNNING, EXTNAME ": working");

    if (http_client_process(&client) != HTTP_CLIENT_OK ||
        http_client_get_transfers(&client, on_response) != HTTP_CLIENT_OK) {
      ereport(WARNING, errmsg(EXTNAME ": error while processing http requests"));
    }

    now = GetCurrentTimestamp();

    // Retire when enough responses have accumulated or the oldest has waited long enough. Also
    // when nothing is in flight anymore, waiting longer would only add latency.
    bool do_retire = num_pending_results > 0 && now >= retire_not_before &&
                     (num_pending_results >= retire_batch_size || now >= retire_time ||
                      client.running_handles == 0);

    bool timer_fill = work_scheduled && now >= work_time;
    if (woken || timer_fill) queue_exhausted = false;

    // Refill: take new requests from the queue to replace the ones which completed. This is done
    // in the same transaction which retires them.
    int capacity = 0;
    if ((woken || timer_fill || do_retire) && !queue_exhausted) {
      capacity = Max(0, guc_batch_size - client.running_handles);
    }

    if (do_retire || capacity > 0) {
      int    num_requests = 0;
      uint64 num_expired  = 0;

      ExtensionLockStatus status =
          run_cycle(&client, do_retire, capacity, &num_requests, &num_expired);

      if (status == EXTENSION_BUSY) {
        // Try again later, nothing was lost
        retire_not_before = TimestampTzPlusMilliseconds(now, RECHECK_INTERVAL_MS);
        work_scheduled    = true;
        work_time         = retire_not_before;
      } else if (capacity > 0) {
        queue_exhausted = num_requests < capacity;

        // Look again later, there might be more to do, e.g. more responses to expire
        work_scheduled = num_requests > 0 || num_expired > 0;
        work_time      = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), RECHECK_INTERVAL_MS);
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
