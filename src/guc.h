#ifndef PG_HTTP_GUC_H
#define PG_HTTP_GUC_H

#include "pg_prelude.h"

// Database the background worker connects to
extern char *guc_database_name;

// User the background worker connects as
extern char *guc_username;

// Time interval after which completed requests and their responses are deleted
extern char *guc_ttl;

// Maximum number of requests in flight at once, and of responses expired per worker cycle
extern int guc_batch_size;

// Number of finished requests which make the worker retire them, 0 means half of batch_size
extern int guc_retire_batch_size;

// Maximum time a finished request waits to be retired
extern int guc_retire_timeout_ms;

// Upper bound for the timeout_milliseconds of a request
extern int guc_max_timeout_ms;

/*
 * Registers all the GUCs needed by pg_http
 */
void register_gucs(void);

#endif /* PG_HTTP_GUC_H */
