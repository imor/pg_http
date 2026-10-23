#include "guc.h"
#include "pg_prelude.h"

char *guc_database_name;
char *guc_username;

void register_gucs(void) {
  DefineCustomStringVariable(EXTNAME ".database_name",
                             "Database to which background worker will connect", NULL,
                             &guc_database_name, "postgres", PGC_SU_BACKEND, 0, NULL, NULL, NULL);

  DefineCustomStringVariable(EXTNAME ".username", "User as which background worker will connect",
                             NULL, &guc_username, NULL, PGC_SU_BACKEND, 0, NULL, NULL, NULL);
}
