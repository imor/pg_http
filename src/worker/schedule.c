#include "schedule.h"
#include "guc.h"
#include "pending.h"

#include <utils/timestamp.h>

/*
 * The time to wait before looking at the queue again after work was done, and before retrying
 * to retire results when the tables were locked by someone else
 */
#define RECHECK_INTERVAL_MS 1000

static int retire_batch_size(void) {
  return guc_retire_batch_size > 0 ? guc_retire_batch_size : Max(1, guc_batch_size / 2);
}

/*
 * When the pending results must be retired at the latest. Only meaningful if there are pending
 * results.
 */
static TimestampTz retire_deadline(const Schedule *schedule) {
  TimestampTz deadline =
      TimestampTzPlusMilliseconds(pending_results_oldest_time(), guc_retire_timeout_ms);
  return Max(deadline, schedule->retire_not_before);
}

/*
 * Retire when enough results have accumulated or the oldest has waited long enough. Also when
 * nothing is in flight anymore, waiting longer would only add latency.
 */
static bool should_retire(const Schedule *schedule, int running_handles, TimestampTz now) {
  int num_pending = pending_results_count();
  if (num_pending == 0 || now < schedule->retire_not_before) return false;

  return num_pending >= retire_batch_size() || now >= retire_deadline(schedule) ||
         running_handles == 0;
}

Schedule schedule_init(void) {
  return (Schedule){.fill_scheduled    = true,
                    .fill_time         = GetCurrentTimestamp(),
                    .queue_exhausted   = false,
                    .retire_not_before = 0};
}

long schedule_timeout_ms(const Schedule *schedule) {
  TimestampTz now        = GetCurrentTimestamp();
  long        timeout_ms = -1;

  if (schedule->fill_scheduled) {
    timeout_ms = (long)TimestampDifferenceMilliseconds(now, schedule->fill_time);
  }

  if (pending_results_count() > 0) {
    long until_retire = (long)TimestampDifferenceMilliseconds(now, retire_deadline(schedule));
    if (timeout_ms < 0 || until_retire < timeout_ms) timeout_ms = until_retire;
  }

  return timeout_ms;
}

bool schedule_has_work(const Schedule *schedule) {
  return schedule->fill_scheduled || pending_results_count() > 0;
}

ExchangePlan schedule_plan(Schedule *schedule, int running_handles, bool woken) {
  TimestampTz  now      = GetCurrentTimestamp();
  bool         fill_due = schedule->fill_scheduled && now >= schedule->fill_time;
  ExchangePlan plan     = {.retire = should_retire(schedule, running_handles, now), .capacity = 0};

  if (woken || fill_due) schedule->queue_exhausted = false;

  if ((woken || fill_due || plan.retire) && !schedule->queue_exhausted) {
    plan.capacity = Max(0, guc_batch_size - running_handles);
  }

  return plan;
}

void schedule_after_exchange(Schedule *schedule, ExchangePlan plan, ExchangeOutcome outcome) {
  TimestampTz recheck_time =
      TimestampTzPlusMilliseconds(GetCurrentTimestamp(), RECHECK_INTERVAL_MS);

  if (outcome.status == EXTENSION_BUSY) {
    // Try again later, nothing was lost
    schedule->retire_not_before = recheck_time;
    schedule->fill_scheduled    = true;
    schedule->fill_time         = recheck_time;
  } else if (plan.capacity > 0) {
    schedule->queue_exhausted = outcome.num_requests < plan.capacity;

    // Look again later, there might be more to do, e.g. more responses to expire
    schedule->fill_scheduled = outcome.num_requests > 0 || outcome.num_expired > 0;
    schedule->fill_time      = recheck_time;
  }
}
