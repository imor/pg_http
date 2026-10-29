#ifndef PG_HTTP_TABLE_QUEUE_H
#define PG_HTTP_TABLE_QUEUE_H

#include "http/request.h"
#include "http/result.h"
#include "pg_prelude.h"

typedef enum {
  HTTP_TABLE_QUEUE_OK    = 0,
  HTTP_TABLE_QUEUE_ERROR = -1,
} TableQueueStatus;

/*
 * Dequeues at most `num_entries` from the request queue and returns them in
 * the `requests` array. The returned rows are atomically marked as in flight
 * so they are never returned again. `num_requests` will contain the actual
 * number of entries which can be less than or equal to `num_entries`.
 *
 * The caller must have connected to SPI.
 * The `requests` array will be allocated in the memory context active
 * before connecting to SPI. Each request is allocated in its own child context of it.
 */
TableQueueStatus dequeue(int num_entries, HttpRequest **requests, int *num_requests);

/*
 * Mark all in flight requests as not in flight so they can be retried.
 * Meant to be called when the worker starts.
 *
 * The caller must have connected to SPI.
 */
TableQueueStatus reset_in_flight(void);

/*
 * Retire a batch of results: saves their responses in http.responses and marks
 * the requests as completed. The requests are not deleted, see delete_expired.
 *
 * The caller must have connected to SPI.
 */
TableQueueStatus retire(HttpResult **results, int num_results);

/*
 * Deletes at most `max_to_delete` responses which are older than the interval `ttl`, along with
 * their requests. `num_deleted` will contain the number of deleted responses.
 *
 * The caller must have connected to SPI.
 */
TableQueueStatus delete_expired(const char *ttl, int max_to_delete, uint64 *num_deleted);

typedef enum {
  // The extension tables don't exist (anymore)
  EXTENSION_ABSENT,
  // The tables exist but are locked by someone else, e.g. DROP EXTENSION or TRUNCATE are running
  EXTENSION_BUSY,
  // The tables exist and are now locked by us until the end of the transaction
  EXTENSION_LOCKED,
} ExtensionLockStatus;

/*
 * Tries to lock the extension's tables in the current transaction without waiting, so that a
 * DROP EXTENSION waits for the transaction to finish, but we never wait for a user holding a
 * lock for a long time. `requests_relid` is set to the oid of the requests table if locked.
 */
ExtensionLockStatus lock_extension_tables(Oid *requests_relid);

#endif /* PG_HTTP_TABLE_QUEUE_H */
