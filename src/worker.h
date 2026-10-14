#ifndef PG_HTTP_WORKER_H
#define PG_HTTP_WORKER_H

#include "pg_prelude.h"

/*
 * The main entry point for the pg_http background worker.
 */
PGDLLEXPORT void pg_http_main(Datum main_arg);

#endif /* PG_HTTP_WORKER_H */
