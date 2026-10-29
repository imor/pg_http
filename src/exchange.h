#ifndef PG_HTTP_EXCHANGE_H
#define PG_HTTP_EXCHANGE_H

#include "http/client.h"
#include "pg_prelude.h"
#include "queue/table.h"

/*
 * An exchange with the queue: the pending results are retired and new requests are taken from
 * the queue to replace them, in one transaction. The new requests are then sent.
 */

/*
 * What an exchange has to do.
 */
typedef struct {
  // Retire the pending results
  bool retire;

  // The maximum number of requests to read from the queue, 0 to not read any
  int capacity;
} ExchangePlan;

/*
 * What an exchange found.
 */
typedef struct {
  ExtensionLockStatus status;
  int                 num_requests;
  uint64              num_expired;
} ExchangeOutcome;

/*
 * Runs an exchange. Also deletes the expired responses when reading from the queue.
 *
 * If the tables were locked by someone else, nothing was done and the pending results are kept.
 */
ExchangeOutcome exchange_with_queue(HttpClient *client, ExchangePlan plan);

#endif /* PG_HTTP_EXCHANGE_H */
