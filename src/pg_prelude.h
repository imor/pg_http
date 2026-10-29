#ifndef PG_PRELUDE_H
#define PG_PRELUDE_H

// postgres.h must be included first, otherwise the build breaks
#include <postgres.h>

#include <access/xact.h>
#include <c.h>
#include <catalog/pg_type.h>
#include <commands/extension.h>
#include <executor/spi.h>
#include <fmgr.h>
#include <lib/stringinfo.h>
#include <miscadmin.h>
#include <pgstat.h>
#include <postmaster/bgworker.h>
#include <postmaster/interrupt.h>
#include <storage/ipc.h>
#include <storage/latch.h>
#include <storage/lwlock.h>
#include <storage/shmem.h>
#include <tcop/tcopprot.h>
#include <utils/array.h>
#include <utils/backend_status.h>
#include <utils/builtins.h>
#include <utils/fmgroids.h>
#include <utils/guc.h>
#include <utils/memutils.h>
#include <utils/snapmgr.h>
#include <utils/wait_event.h>

/*
 * Like DatumGetArrayTypeP, without tripping -Wcast-align
 */
static inline ArrayType *datum_get_array(Datum datum) {
  return (ArrayType *)(void *)PG_DETOAST_DATUM(datum);
}

#endif /* PG_PRELUDE_H */