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
 * the `requests` array. `num_requests` will contain the actual number of
 * entries which can be less than or equal to `num_entries`.
 */
TableQueueStatus dequeue(int num_entries, HttpRequest *requests, int *num_requests);

/*
 * Retire responses in the `responses` array by saving their results.
 * `num_responses` represents the number of responses in the array.
 */
TableQueueStatus retire(HttpResponse *responses, int num_responses);

/*
 * Delete entries which have completed and are older than `older_than_secs`
 * seconds. At most `max_to_delete` entries will be deleted.
 */
TableQueueStatus delete_expired(int older_than_secs, int max_to_delete);

#endif /* PG_HTTP_TABLE_QUEUE_H */
