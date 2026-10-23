#include "table.h"
#include "pg_prelude.h"

static SPIPlanPtr  dequeue_query_plan = NULL;
static const char *dequeue_query      = "\
    update http.requests \
    set in_flight = true \
    where id in ( \
      select id \
      from http.requests \
      where not in_flight \
      order by id \
      limit $1 \
    ) \
    returning id, url \
";

static SPIPlanPtr  reset_query_plan = NULL;
static const char *reset_query      = "\
    update http.requests \
    set in_flight = false \
    where in_flight \
";

static SPIPlanPtr  retire_query_plan = NULL;
static const char *retire_query      = "\
    delete from http.requests \
    where id = $1 \
";

/*
 * Prepares and keeps a plan for the given sql source and returns the
 * kept plan
 */
static SPIPlanPtr prepare_and_keep_plan(const char *sql, int nargs, Oid *argtypes) {
  SPIPlanPtr prepared = SPI_prepare(sql, nargs, argtypes);
  if (prepared == NULL)
    ereport(ERROR, errmsg("SPI_prepare failed: %s", SPI_result_code_string(SPI_result)));

  int ret_code = SPI_keepplan(prepared);
  if (ret_code != 0)
    ereport(ERROR, errmsg("SPI_keepplan failed: %s", SPI_result_code_string(ret_code)));

  return prepared;
}

TableQueueStatus dequeue(int num_entries, HttpRequest **requests, int *num_requests) {
  if (dequeue_query_plan == NULL) {
    dequeue_query_plan = prepare_and_keep_plan(dequeue_query, 1, (Oid[]){INT4OID});
  }

  int ret_code =
      SPI_execute_plan(dequeue_query_plan, (Datum[]){Int32GetDatum(num_entries)}, NULL, false, 0);

  if (ret_code != SPI_OK_UPDATE_RETURNING) {
    ereport(ERROR,
            errmsg("error reading from http.requests: %s", SPI_result_code_string(ret_code)));
  }

  uint64 processed = SPI_processed;
  int    count     = (int)Min(processed, (uint64)num_entries);

  // We allocate in the memory context which was active before connecting to SPI.
  HttpRequest *result = count > 0 ? SPI_palloc(sizeof(HttpRequest) * (size_t)count) : NULL;

  for (int i = 0; i < count; i++) {
    HeapTuple tuple = SPI_tuptable->vals[i];

    bool  isnull;
    Datum id = SPI_getbinval(tuple, SPI_tuptable->tupdesc, 1, &isnull);
    if (isnull) ereport(ERROR, errmsg("unexpected null id in http.requests"));

    char *url = SPI_getvalue(tuple, SPI_tuptable->tupdesc, 2);
    if (url == NULL) ereport(ERROR, errmsg("unexpected null url in http.requests"));

    size_t url_len  = strlen(url) + 1;
    char  *url_copy = SPI_palloc(url_len);
    memcpy(url_copy, url, url_len);
    pfree(url);

    result[i] = (HttpRequest){.id = DatumGetInt64(id), .url = url_copy};
  }

  *requests     = result;
  *num_requests = count;

  SPI_freetuptable(SPI_tuptable);

  return HTTP_TABLE_QUEUE_OK;
}

TableQueueStatus reset_in_flight(void) {
  if (reset_query_plan == NULL) {
    reset_query_plan = prepare_and_keep_plan(reset_query, 0, NULL);
  }

  int ret_code = SPI_execute_plan(reset_query_plan, NULL, NULL, false, 0);
  if (ret_code != SPI_OK_UPDATE) {
    ereport(ERROR,
            errmsg("error resetting http.requests: %s", SPI_result_code_string(ret_code)));
  }

  return HTTP_TABLE_QUEUE_OK;
}

TableQueueStatus retire(HttpResponse *responses, int num_responses) {
  if (retire_query_plan == NULL) {
    retire_query_plan = prepare_and_keep_plan(retire_query, 1, (Oid[]){INT8OID});
  }

  for (int i = 0; i < num_responses; i++) {
    int ret_code = SPI_execute_plan(
        retire_query_plan, (Datum[]){Int64GetDatum(responses[i].request_id)}, NULL, false, 0);
    if (ret_code != SPI_OK_DELETE) {
      ereport(ERROR,
              errmsg("error deleting from http.requests: %s", SPI_result_code_string(ret_code)));
    }
  }

  return HTTP_TABLE_QUEUE_OK;
}

TableQueueStatus delete_expired(__attribute__((unused)) int older_than_secs,
                                __attribute__((unused)) int max_to_delete) {
  return HTTP_TABLE_QUEUE_OK;
}
