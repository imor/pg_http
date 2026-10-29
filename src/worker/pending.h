#ifndef PG_HTTP_PENDING_H
#define PG_HTTP_PENDING_H

#include "http/result.h"
#include "pg_prelude.h"

/*
 * The results of finished requests waiting to be retired, i.e. written to the tables.
 */

/*
 * Adds a result to the ones waiting to be retired, which take ownership of it.
 */
void pending_results_add(HttpResult *result);

int pending_results_count(void);

/*
 * When the oldest pending result was added, only meaningful if there are pending results.
 */
TimestampTz pending_results_oldest_time(void);

/*
 * Writes the pending results to the tables. Only the results whose request was read from the
 * current incarnation of the extension are retired, the others belong to a dropped extension.
 *
 * Must be called in a transaction holding the extension's table locks. The results are kept, call
 * pending_results_drop once the transaction committed.
 */
void pending_results_retire(Oid requests_relid);

/*
 * Destroys all the pending results.
 */
void pending_results_drop(void);

#endif /* PG_HTTP_PENDING_H */
