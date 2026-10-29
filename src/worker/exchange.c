#include "exchange.h"
#include "guc.h"
#include "pending.h"
#include "txn.h"

/*
 * The state of an exchange's transaction: its plan, and what it read from the tables.
 */
typedef struct {
  ExchangePlan    plan;
  ExchangeOutcome outcome;
  HttpRequest    *requests;
  Oid             requests_relid;
} Exchange;

/*
 * Whether the worker still has to reset the requests marked as in flight by a previous
 * worker instance.
 */
static bool reset_pending = true;

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
 * Retries the requests which were marked as in flight by a previous worker instance, this
 * guarantees at-least-once semantics. Only done once per worker instance.
 */
static void reset_previous_in_flight(void) {
  if (!reset_pending) return;

  if (reset_in_flight() != HTTP_TABLE_QUEUE_OK) {
    ereport(ERROR, errmsg(EXTNAME ": could not reset in flight requests"));
  }
  reset_pending = false;
}

static void expire_responses(uint64 *num_expired) {
  if (delete_expired(guc_ttl, guc_batch_size, num_expired) != HTTP_TABLE_QUEUE_OK) {
    ereport(ERROR, errmsg(EXTNAME ": could not delete expired responses"));
  }
}

static void dequeue_requests(int capacity, HttpRequest **requests, int *num_requests) {
  if (dequeue(capacity, requests, num_requests) != HTTP_TABLE_QUEUE_OK) {
    ereport(ERROR, errmsg(EXTNAME ": could not read requests from the queue"));
  }
}

/*
 * Retires the pending results, deletes the expired responses and takes the next requests from the
 * queue, all in one transaction.
 */
static void exchange_txn(void *arg) {
  Exchange *exchange = (Exchange *)arg;

  exchange->outcome.status = begin_extension_work(&exchange->requests_relid);
  if (exchange->outcome.status != EXTENSION_LOCKED) return;

  if (exchange->plan.retire) pending_results_retire(exchange->requests_relid);

  if (exchange->plan.capacity > 0) {
    reset_previous_in_flight();
    expire_responses(&exchange->outcome.num_expired);
    dequeue_requests(exchange->plan.capacity, &exchange->requests, &exchange->outcome.num_requests);
  }
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
      pending_results_add(http_result_rejected(request, requests_relid, rejection));
      pfree(rejection);
      continue;
    }

    HttpTransfer *transfer = http_transfer_create(request, requests_relid);
    if (transfer == NULL) {
      ereport(ERROR, errmsg(EXTNAME ": could not initialize http transfer"));
    }

    if (http_client_send(client, transfer) != HTTP_CLIENT_OK) {
      ereport(WARNING, errmsg(EXTNAME ": could not start request to %s", transfer->request.url));
      pending_results_add(http_transfer_abort(transfer, "could not start the request"));
    }
  }
}

/*
 * The transaction is only held open while reading from the table. The requests are allocated in
 * a separate memory context so they outlive it.
 */
ExchangeOutcome exchange_with_queue(HttpClient *client, ExchangePlan plan) {
  // The Internal variant avoids the GNU statement expression in the macro
  MemoryContext batch_context =
      AllocSetContextCreateInternal(TopMemoryContext, EXTNAME " batch", ALLOCSET_DEFAULT_MINSIZE,
                                    ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE);

  Exchange exchange = {.plan    = plan,
                       .outcome = {.status = EXTENSION_ABSENT, .num_requests = 0, .num_expired = 0},
                       .requests       = NULL,
                       .requests_relid = InvalidOid};

  // Connect to SPI in the batch context to make the requests survive the commit
  run_in_transaction(exchange_txn, &exchange, batch_context);

  if (exchange.outcome.status != EXTENSION_BUSY) {
    if (plan.retire) pending_results_drop();
    send_requests(client, exchange.requests, exchange.outcome.num_requests,
                  exchange.requests_relid);
  }

  MemoryContextDelete(batch_context);

  return exchange.outcome;
}
