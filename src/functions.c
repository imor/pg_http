#include "pg_prelude.h"
#include "shmem.h"

#include <curl/curl.h>
#include <utils/fmgroids.h>
#include <utils/guc.h>

PG_FUNCTION_INFO_V1(_urlencode_string);
PG_FUNCTION_INFO_V1(_encode_url_with_params_array);
PG_FUNCTION_INFO_V1(worker_restart);
PG_FUNCTION_INFO_V1(wait_until_running);
PG_FUNCTION_INFO_V1(wake);

Datum _urlencode_string(PG_FUNCTION_ARGS) {
  if (PG_ARGISNULL(0)) PG_RETURN_NULL();

  char *str     = text_to_cstring(PG_GETARG_TEXT_PP(0));
  char *encoded = curl_easy_escape(NULL, str, (int)strlen(str));
  pfree(str);

  if (encoded == NULL) PG_RETURN_NULL();

  text *result = cstring_to_text(encoded);
  curl_free(encoded);
  PG_RETURN_TEXT_P(result);
}

Datum _encode_url_with_params_array(PG_FUNCTION_ARGS) {
  if (PG_ARGISNULL(0) || PG_ARGISNULL(1)) PG_RETURN_NULL();

  char      *url    = text_to_cstring(PG_GETARG_TEXT_PP(0));
  ArrayType *params = datum_get_array(PG_GETARG_DATUM(1));

  CURLU    *handle = curl_url();
  CURLUcode rc     = curl_url_set(handle, CURLUPART_URL, url, 0);
  if (rc != CURLUE_OK) {
    curl_url_cleanup(handle);
    ereport(ERROR, errmsg("invalid URL \"%s\": %s", url, curl_url_strerror(rc)));
  }

  ArrayIterator iterator = array_create_iterator(params, 0, NULL);
  Datum         value;
  bool          isnull;
  while (array_iterate(iterator, &value, &isnull)) {
    if (isnull) continue;

    char *param = TextDatumGetCString(value);
    rc          = curl_url_set(handle, CURLUPART_QUERY, param, CURLU_APPENDQUERY);
    if (rc != CURLUE_OK) {
      curl_url_cleanup(handle);
      ereport(ERROR, errmsg("invalid URL \"%s\": %s", param, curl_url_strerror(rc)));
    }
    pfree(param);
  }
  array_free_iterator(iterator);

  char *full_url = NULL;
  rc             = curl_url_get(handle, CURLUPART_URL, &full_url, 0);
  curl_url_cleanup(handle);
  if (rc != CURLUE_OK) {
    ereport(ERROR, errmsg("failed to encode URL \"%s\": %s", url, curl_url_strerror(rc)));
  }

  pfree(url);
  text *result = cstring_to_text(full_url);
  curl_free(full_url);
  PG_RETURN_TEXT_P(result);
}

Datum worker_restart(__attribute__((unused)) PG_FUNCTION_ARGS) {
  // Reload the config so the new worker sees any pending ALTER SYSTEM changes
  bool result = DatumGetBool(DirectFunctionCall1(pg_reload_conf, (Datum)0));

  restart_workers();

  PG_RETURN_BOOL(result);
}

Datum wait_until_running(__attribute__((unused)) PG_FUNCTION_ARGS) {
  WorkerSharedState *state = worker_shared_state;

  if (!all_workers_running()) {
    ConditionVariablePrepareToSleep(&state->cv);
    while (!all_workers_running()) {
      ConditionVariableSleep(&state->cv, PG_WAIT_EXTENSION);
    }
    ConditionVariableCancelSleep();
  }

  PG_RETURN_VOID();
}

/*
 * Set when `wake` was called in the current transaction
 */
static bool wake_at_commit_pending = false;

/*
 * RegisterXactCallback never deduplicates, so the callback must be registered
 * at most once per backend.
 */
static bool wake_callback_registered = false;

/*
 * The worker is only woken at commit time, so that e.g. inserting thousands of
 * requests in one transaction wakes it once, and only if the transaction committed.
 */
static void wake_xact_callback(XactEvent event, __attribute__((unused)) void *arg) {
  switch (event) {
  case XACT_EVENT_COMMIT:
  case XACT_EVENT_PARALLEL_COMMIT:
    if (wake_at_commit_pending) {
      wake_workers();
      wake_at_commit_pending = false;
    }
    break;
  case XACT_EVENT_PREPARE:
  case XACT_EVENT_ABORT:
  case XACT_EVENT_PARALLEL_ABORT     : wake_at_commit_pending = false; break;
  case XACT_EVENT_PRE_COMMIT         :
  case XACT_EVENT_PARALLEL_PRE_COMMIT:
  case XACT_EVENT_PRE_PREPARE        : break;
  }
}

Datum wake(__attribute__((unused)) PG_FUNCTION_ARGS) {
  if (!wake_callback_registered) {
    RegisterXactCallback(wake_xact_callback, NULL);
    wake_callback_registered = true;
  }

  wake_at_commit_pending = true;

  PG_RETURN_VOID();
}
