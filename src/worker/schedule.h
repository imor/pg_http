#ifndef PG_HTTP_SCHEDULE_H
#define PG_HTTP_SCHEDULE_H

#include "exchange.h"
#include "pg_prelude.h"

/*
 * When the worker has to do work without being woken, and what.
 */
typedef struct {
  // Whether a look at the queue is scheduled, and when
  bool        fill_scheduled;
  TimestampTz fill_time;

  // Set once a read from the queue found less requests than it could take, i.e. the queue is
  // empty. Retiring doesn't refill from the queue then, until a wake or the recheck.
  bool queue_exhausted;

  // Retiring is not attempted before this time, set when the tables were locked by someone else
  TimestampTz retire_not_before;
} Schedule;

/*
 * A schedule whose first exchange is due right away: requests could have been added while no
 * worker was running, and wakes sent then are lost.
 */
Schedule schedule_init(void);

/*
 * How long to sleep until the next timed work, -1 to sleep until woken.
 */
long schedule_timeout_ms(const Schedule *schedule);

/*
 * Whether timed work is waiting, either a look at the queue or results to retire.
 */
bool schedule_has_work(const Schedule *schedule);

/*
 * Decides what the next exchange has to do. New requests are taken from the queue to replace the
 * ones which completed, in the same transaction which retires them.
 */
ExchangePlan schedule_plan(Schedule *schedule, int running_handles, bool woken);

/*
 * Schedules the next work after an exchange.
 */
void schedule_after_exchange(Schedule *schedule, ExchangePlan plan, ExchangeOutcome outcome);

#endif /* PG_HTTP_SCHEDULE_H */
