#include "pg_prelude.h"

#include <curl/curl.h>
#include <stdbool.h>
#include <string.h>

PG_MODULE_MAGIC;

/*
 * The function called by postmaster during initialization.
 * 
 * This function should:
 * 
 * * Register any GUCs.
 * * Register pg_http's background worker.
 * 
*/
void _PG_init(void)
{
  // If it is a binary upgrade we return early because running a background
  // worker in binary upgrade mode might interfere with the upgrade.
  if (IsBinaryUpgrade)
  {
    return;
  }

  // Ensure that pg_http is configured in shared_preload_libraries
  if (!process_shared_preload_libraries_in_progress)
  {
    ereport(ERROR, errmsg("pg_http is not in shared_preload_libraries"),
            errhint("Add pg_http to the shared_preload_libraries GUC in postgresql.conf."));
  }
}
