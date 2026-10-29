#include "pending.h"
#include "queue/table.h"

#include <utils/timestamp.h>

/*
 * The initial capacity of the array of results waiting to be retired
 */
#define PENDING_RESULTS_INITIAL_SIZE 256

/*
 * The array grows when results can't be retired for a while, e.g. while a user holds a lock on
 * the tables.
 */
static HttpResult **pending_results      = NULL;
static int          num_pending_results  = 0;
static int          pending_results_size = 0;

static TimestampTz first_pending_time = 0;

static void grow(void) {
  int new_size =
      pending_results_size == 0 ? PENDING_RESULTS_INITIAL_SIZE : pending_results_size * 2;
  MemoryContext old_context = MemoryContextSwitchTo(TopMemoryContext);
  pending_results           = pending_results == NULL
                                  ? palloc(sizeof(HttpResult *) * (size_t)new_size)
                                  : repalloc(pending_results, sizeof(HttpResult *) * (size_t)new_size);
  MemoryContextSwitchTo(old_context);
  pending_results_size = new_size;
}

void pending_results_add(HttpResult *result) {
  if (num_pending_results == pending_results_size) grow();

  if (num_pending_results == 0) first_pending_time = GetCurrentTimestamp();

  pending_results[num_pending_results++] = result;
}

int pending_results_count(void) {
  return num_pending_results;
}

TimestampTz pending_results_oldest_time(void) {
  return first_pending_time;
}

void pending_results_retire(Oid requests_relid) {
  HttpResult **current = palloc(sizeof(HttpResult *) * (size_t)num_pending_results);
  int          count   = 0;
  for (int i = 0; i < num_pending_results; i++) {
    if (pending_results[i]->requests_relid == requests_relid) current[count++] = pending_results[i];
  }

  if (count > 0 && retire(current, count) != HTTP_TABLE_QUEUE_OK) {
    ereport(ERROR, errmsg(EXTNAME ": could not retire requests"));
  }
}

void pending_results_drop(void) {
  for (int i = 0; i < num_pending_results; i++) {
    http_result_destroy(pending_results[i]);
  }
  num_pending_results = 0;
}
