#ifndef PG_HTTP_GUC_H
#define PG_HTTP_GUC_H

#include "pg_prelude.h"

// Database the background worker connects to
extern char *guc_database_name;

// User the background worker connects as
extern char *guc_username;

/*
 * Registers all the GUCs needed by pg_http
 */
void register_gucs(void);

#endif /* PG_HTTP_GUC_H */
