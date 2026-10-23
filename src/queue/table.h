#ifndef PG_HTTP_TABLE_QUEUE_H
#define PG_HTTP_TABLE_QUEUE_H

#include "http/request.h"
#include "http/response.h"

typedef enum {
  HTTP_TABLE_QUEUE_OK    = 0,
  HTTP_TABLE_QUEUE_ERROR = -1,
} TableQueueStatus;

/*
 * Dequeue at most `num_entries` from the request queue and return them in
 * the `requests` array. The returned rows are atomically marked as in flight
 * so they are never returned again. `num_requests` will contain the actual number of
 * entries which can be less than or equal to `num_entries`.
 *
 * The caller must have connected to SPI with and `SPI_connect` call.
 * The `requests` array will be allocated in the memory context active
 * before connecting to SPI.
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
 * Retire responses in the `responses` array by deleting the requests they
 * belong to. `num_responses` represents the number of responses in the array.
 *
 * The caller must have connected to SPI.
 */
TableQueueStatus retire(HttpResponse *responses, int num_responses);

/*
 * Delete entries which have completed and are older than `older_than_secs`
 * seconds. At most `max_to_delete` entries will be deleted.
 */
TableQueueStatus delete_expired(int older_than_secs, int max_to_delete);

#endif /* PG_HTTP_TABLE_QUEUE_H */
