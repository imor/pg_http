#include "table.h"
#include "pg_prelude.h"

#include <catalog/namespace.h>
#include <storage/lmgr.h>
#include <string.h>
#include <utils/fmgroids.h>
#include <utils/lsyscache.h>
#include <utils/syscache.h>
#include <utils/timestamp.h>

static SPIPlanPtr  dequeue_query_plan = NULL;
static const char *dequeue_query      = "\
    update http.requests \
    set status = 'in_flight' \
    where id in ( \
      select id \
      from http.requests \
      where status = 'queued' \
      order by id \
      limit $1 \
    ) \
    returning id, method, url, timeout_milliseconds, \
      array( \
        select key || ': ' || value \
        from jsonb_each_text(case when jsonb_typeof(headers) = 'object' then headers end) \
      ), \
      body \
";

static SPIPlanPtr  reset_query_plan = NULL;
static const char *reset_query      = "\
    update http.requests \
    set status = 'queued' \
    where status = 'in_flight' \
";

static SPIPlanPtr  retire_query_plan = NULL;
static const char *retire_query      = "\
    with inserted as ( \
      insert into http.responses (id, status_code, headers, body, error_type, error_msg) \
      select id, status_code, headers, body, error_type::http.error_type, error_msg \
      from unnest($1::int8[], $2::int4[], $3::jsonb[], $4::text[], $5::text[], $6::text[]) \
        as t(id, status_code, headers, body, error_type, error_msg) \
      on conflict (id) do nothing \
      returning id \
    ) \
    update http.requests \
    set status = 'completed' \
    where id in (select id from inserted) \
";

static SPIPlanPtr  delete_expired_query_plan = NULL;
static const char *delete_expired_query      = "\
    with expired as ( \
      select id \
      from http.responses \
      where created < now() - $1 \
      order by created \
      limit $2 \
    ), deleted_requests as ( \
      delete from http.requests where id in (select id from expired) \
    ) \
    delete from http.responses where id in (select id from expired) \
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

// The varlena macros cast to a more strictly aligned type, which is fine for a detoasted datum
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
/*
 * Copies the bytes of a bytea into `context`. The copy is NUL terminated, but its length doesn't
 * include the terminator.
 */
static char *copy_bytea(MemoryContext context, Datum datum, size_t *len) {
  bytea *bytes = DatumGetByteaPP(datum);
  *len         = VARSIZE_ANY_EXHDR(bytes);
  char *copy   = MemoryContextAlloc(context, *len + 1);
  memcpy(copy, VARDATA_ANY(bytes), *len);
  copy[*len] = '\0';
  return copy;
}
#pragma GCC diagnostic pop

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
    // Each request gets its own context, which a transfer takes over without copying the request.
    // Until then it is a child of the context active before connecting to SPI.
    MemoryContext context = AllocSetContextCreateInternal(
        GetMemoryChunkContext(result), EXTNAME " request", ALLOCSET_SMALL_MINSIZE,
        ALLOCSET_SMALL_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE);

    HeapTuple tuple = SPI_tuptable->vals[i];
    TupleDesc desc  = SPI_tuptable->tupdesc;

    bool  isnull;
    Datum id = SPI_getbinval(tuple, desc, 1, &isnull);
    if (isnull) ereport(ERROR, errmsg("unexpected null id in http.requests"));

    char *method = SPI_getvalue(tuple, desc, 2);
    if (method == NULL) ereport(ERROR, errmsg("unexpected null method in http.requests"));

    char *url = SPI_getvalue(tuple, desc, 3);
    if (url == NULL) ereport(ERROR, errmsg("unexpected null url in http.requests"));

    Datum timeout = SPI_getbinval(tuple, desc, 4, &isnull);
    if (isnull) ereport(ERROR, errmsg("unexpected null timeout_milliseconds in http.requests"));

    result[i] = (HttpRequest){.context              = context,
                              .id                   = DatumGetInt64(id),
                              .method               = MemoryContextStrdup(context, method),
                              .url                  = MemoryContextStrdup(context, url),
                              .timeout_milliseconds = DatumGetInt32(timeout),
                              .headers              = NULL,
                              .body                 = NULL,
                              .body_len             = 0};

    Datum headers = SPI_getbinval(tuple, desc, 5, &isnull);
    if (!isnull) {
      Datum *elements;
      bool  *nulls;
      int    n;
      deconstruct_array(datum_get_array(headers), TEXTOID, -1, false, TYPALIGN_INT, &elements,
                        &nulls, &n);

      for (int j = 0; j < n; j++) {
        if (nulls[j]) continue;
        struct curl_slist *appended =
            curl_slist_append(result[i].headers, TextDatumGetCString(elements[j]));
        if (appended == NULL) {
          curl_slist_free_all(result[i].headers);
          ereport(ERROR, errmsg(EXTNAME ": could not copy the headers of a request"));
        }
        result[i].headers = appended;
      }
    }

    Datum body = SPI_getbinval(tuple, desc, 6, &isnull);
    if (!isnull) {
      size_t len         = 0;
      result[i].body     = copy_bytea(context, body, &len);
      result[i].body_len = len;
    }
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
    ereport(ERROR, errmsg("error resetting http.requests: %s", SPI_result_code_string(ret_code)));
  }

  return HTTP_TABLE_QUEUE_OK;
}

static ArrayType *build_array(Datum *values, bool *nulls, int n, Oid type, int16 len, bool byval,
                              char align) {
  int dims[1] = {n};
  int lbs[1]  = {1};
  return construct_md_array(values, nulls, 1, dims, lbs, type, len, byval, align);
}

TableQueueStatus retire(HttpTransfer **transfers, int num_transfers) {
  if (retire_query_plan == NULL) {
    retire_query_plan = prepare_and_keep_plan(retire_query, 6,
                                              (Oid[]){INT8ARRAYOID, INT4ARRAYOID, JSONBARRAYOID,
                                                      TEXTARRAYOID, TEXTARRAYOID, TEXTARRAYOID});
  }

  if (num_transfers <= 0) return HTTP_TABLE_QUEUE_OK;

  size_t n       = (size_t)num_transfers;
  Datum *ids     = palloc(sizeof(Datum) * n);
  Datum *codes   = palloc(sizeof(Datum) * n);
  Datum *headers = palloc(sizeof(Datum) * n);
  Datum *body    = palloc(sizeof(Datum) * n);
  Datum *etypes  = palloc(sizeof(Datum) * n);
  Datum *errors  = palloc(sizeof(Datum) * n);
  bool  *no_code = palloc(sizeof(bool) * n);
  bool  *no_hdrs = palloc(sizeof(bool) * n);
  bool  *no_body = palloc(sizeof(bool) * n);
  bool  *no_type = palloc(sizeof(bool) * n);
  bool  *no_err  = palloc(sizeof(bool) * n);
  bool  *never   = palloc0(sizeof(bool) * n);

  for (int i = 0; i < num_transfers; i++) {
    HttpResponse *r = &transfers[i]->response;
    ids[i]          = Int64GetDatum(transfers[i]->request.id);
    no_code[i]      = r->status_code == 0;
    codes[i]        = Int32GetDatum(r->status_code);
    no_hdrs[i]      = r->headers == NULL;
    headers[i]      = no_hdrs[i] ? (Datum)0 : JsonbPGetDatum(r->headers);
    no_body[i]      = r->body.len == 0;
    body[i]         = no_body[i] ? (Datum)0
                                 : PointerGetDatum(cstring_to_text_with_len(r->body.data, r->body.len));
    no_type[i]      = r->error_type == NULL;
    etypes[i]       = no_type[i] ? (Datum)0 : CStringGetTextDatum(r->error_type);
    no_err[i]       = r->error_msg == NULL;
    errors[i]       = no_err[i] ? (Datum)0 : CStringGetTextDatum(r->error_msg);
  }

  Datum args[6] = {
    PointerGetDatum(build_array(ids, never, num_transfers, INT8OID, sizeof(int64), true, 'd')),
    PointerGetDatum(build_array(codes, no_code, num_transfers, INT4OID, sizeof(int32), true, 'i')),
    PointerGetDatum(build_array(headers, no_hdrs, num_transfers, JSONBOID, -1, false, 'i')),
    PointerGetDatum(build_array(body, no_body, num_transfers, TEXTOID, -1, false, 'i')),
    PointerGetDatum(build_array(etypes, no_type, num_transfers, TEXTOID, -1, false, 'i')),
    PointerGetDatum(build_array(errors, no_err, num_transfers, TEXTOID, -1, false, 'i')),
  };

  int ret_code = SPI_execute_plan(retire_query_plan, args, NULL, false, 0);
  if (ret_code != SPI_OK_UPDATE) {
    ereport(ERROR, errmsg("error retiring requests in http.requests: %s",
                          SPI_result_code_string(ret_code)));
  }

  for (int i = 0; i < 6; i++)
    pfree(DatumGetPointer(args[i]));
  pfree(ids);
  pfree(codes);
  pfree(headers);
  pfree(body);
  pfree(etypes);
  pfree(errors);
  pfree(no_code);
  pfree(no_hdrs);
  pfree(no_body);
  pfree(no_type);
  pfree(no_err);
  pfree(never);

  return HTTP_TABLE_QUEUE_OK;
}

TableQueueStatus delete_expired(const char *ttl, int max_to_delete, uint64 *num_deleted) {
  if (delete_expired_query_plan == NULL) {
    delete_expired_query_plan =
        prepare_and_keep_plan(delete_expired_query, 2, (Oid[]){INTERVALOID, INT4OID});
  }

  Datum interval = DirectFunctionCall3(interval_in, CStringGetDatum(ttl),
                                       ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));

  int ret_code = SPI_execute_plan(
      delete_expired_query_plan, (Datum[]){interval, Int32GetDatum(max_to_delete)}, NULL, false, 0);
  if (ret_code != SPI_OK_DELETE) {
    ereport(ERROR, errmsg("error deleting expired requests: %s", SPI_result_code_string(ret_code)));
  }

  *num_deleted = SPI_processed;

  return HTTP_TABLE_QUEUE_OK;
}

ExtensionLockStatus lock_extension_tables(Oid *requests_relid) {
  Oid schema = get_namespace_oid("http", true);
  if (!OidIsValid(schema)) return EXTENSION_ABSENT;

  Oid requests  = get_relname_relid("requests", schema);
  Oid responses = get_relname_relid("responses", schema);

  // The schema can exist without the tables, e.g. when another extension is installed into a
  // schema with the same name. Locking an invalid oid would succeed.
  if (!OidIsValid(requests) || !OidIsValid(responses)) return EXTENSION_ABSENT;

  // Holding the locks until the end of the transaction makes DROP EXTENSION wait for us
  if (!ConditionalLockRelationOid(requests, AccessShareLock)) return EXTENSION_BUSY;
  if (!ConditionalLockRelationOid(responses, AccessShareLock)) {
    UnlockRelationOid(requests, AccessShareLock);
    return EXTENSION_BUSY;
  }

  // The tables could have been dropped just before we locked them
  if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(requests)) ||
      !SearchSysCacheExists1(RELOID, ObjectIdGetDatum(responses))) {
    return EXTENSION_ABSENT;
  }

  *requests_relid = requests;
  return EXTENSION_LOCKED;
}
