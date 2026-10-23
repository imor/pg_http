#ifndef PG_PRELUDE_H
#define PG_PRELUDE_H

// postgres.h must be included first, otherwise the build breaks
#include <postgres.h>

#include <access/xact.h>
#include <c.h>
#include <commands/extension.h>
#include <executor/spi.h>
#include <fmgr.h>
#include <lib/stringinfo.h>
#include <miscadmin.h>
#include <postmaster/bgworker.h>
#include <postmaster/interrupt.h>
#include <storage/ipc.h>
#include <storage/latch.h>
#include <storage/lwlock.h>
#include <storage/shmem.h>
#include <tcop/tcopprot.h>
#include <utils/backend_status.h>
#include <utils/memutils.h>
#include <utils/snapmgr.h>
#include <utils/wait_event.h>

#endif /* PG_PRELUDE_H */