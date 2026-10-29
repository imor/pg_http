#include "guc.h"
#include "pg_prelude.h"

#include <utils/fmgroids.h>
#include <utils/guc.h>
#include <utils/timestamp.h>

char *guc_database_name;
char *guc_username;
char *guc_ttl;
int   guc_batch_size;
int   guc_retire_batch_size;
int   guc_retire_timeout_ms;
int   guc_max_timeout_ms;

/*
 * Check hook for the ttl GUC. The value is parsed with interval_in so that an invalid value is
 * rejected at SET/ALTER SYSTEM/config reload time instead of making the worker fail when it
 * deletes expired responses. Negative intervals are rejected since they would expire every
 * response immediately.
 */
static bool check_ttl(char **newval, __attribute__((unused)) void **extra,
                      __attribute__((unused)) GucSource source) {
  if (*newval == NULL) return true;

  MemoryContext ccxt      = CurrentMemoryContext;
  Datum         ttl_datum = (Datum)0;
  bool          parsed    = false;

  PG_TRY();
  {
    ttl_datum = DirectFunctionCall3(interval_in, CStringGetDatum(*newval),
                                    ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
    parsed    = true;
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(ccxt);
    ErrorData *edata = CopyErrorData();
    FlushErrorState();
    GUC_check_errdetail("%s", edata->message);
    FreeErrorData(edata);
  }
  PG_END_TRY();

  if (!parsed) return false;

  Datum zero = DirectFunctionCall3(interval_in, CStringGetDatum("0"), ObjectIdGetDatum(InvalidOid),
                                   Int32GetDatum(-1));

  if (DatumGetInt32(DirectFunctionCall2(interval_cmp, ttl_datum, zero)) < 0) {
    GUC_check_errdetail("\"%s\" is a negative interval", *newval);
    return false;
  }

  return true;
}

void register_gucs(void) {
  DefineCustomStringVariable(EXTNAME ".database_name",
                             "Database to which background worker will connect", NULL,
                             &guc_database_name, "postgres", PGC_SU_BACKEND, 0, NULL, NULL, NULL);

  DefineCustomStringVariable(EXTNAME ".username", "User as which background worker will connect",
                             NULL, &guc_username, NULL, PGC_SU_BACKEND, 0, NULL, NULL, NULL);

  DefineCustomStringVariable(
      EXTNAME ".ttl", "time to live for completed requests/response",
      "time interval after which completed request/response rows are deleted", &guc_ttl, "6 hours",
      PGC_SIGHUP, 0, check_ttl, NULL, NULL);

  DefineCustomIntVariable(EXTNAME ".batch_size",
                          "maximum number of requests the background worker has in flight at once",
                          NULL, &guc_batch_size, 200, 1, PG_INT16_MAX, PGC_SIGHUP, 0, NULL, NULL,
                          NULL);

  DefineCustomIntVariable(EXTNAME ".retire_batch_size",
                          "number of finished requests which make the worker save their responses",
                          "0 means half of batch_size", &guc_retire_batch_size, 0, 0, PG_INT16_MAX,
                          PGC_SIGHUP, 0, NULL, NULL, NULL);

  DefineCustomIntVariable(EXTNAME ".retire_timeout_ms",
                          "maximum time a finished request waits before its response is saved",
                          NULL, &guc_retire_timeout_ms, 100, 0, PG_INT32_MAX, PGC_SIGHUP, 0, NULL,
                          NULL, NULL);

  DefineCustomIntVariable(
      EXTNAME ".max_timeout_ms", "upper bound for the timeout_milliseconds of a request", NULL,
      &guc_max_timeout_ms, 600000, 1, PG_INT32_MAX, PGC_SUSET, 0, NULL, NULL, NULL);
}
