# Benchmarks

Load test comparing pg_http with pg_net. All numbers are raw results of single runs, nothing is
averaged. Reproduce with `http-loadtest` (see [nix/loadtest.sh](nix/loadtest.sh)).

## Variants

* **pg_net**: `../pg_net` at its current checkout (0.20.4 / 0.21.0 SQL).
* **pg_http**: this working tree.

## Environment

* Apple M2 Pro, 12 cores, 32 GB, macOS 27.0.1 (Darwin 27.0.0)
* PostgreSQL 17.0 (xpg, `xpg -v 17`, no cassert)
* Requests go over loopback to nginx server
* Default settings unless stated: `retire_batch_size=0` (half of `batch_size`),
  `retire_timeout_ms=100`, `max_timeout_ms=600000`, `ttl=6 hours`, request timeout 5000 ms
* `time_taken` is the time from the end of the inserts until all N responses are in the
  responses table. CPU is the worker's cumulative CPU time, RSS its maximum resident size
  (sampled every 200 ms, so a peak can be missed).
* The loadtest waits for all N responses (`count(*) = N`), not for the last request id.
* Between runs the script waits for sockets in `TIME_WAIT` to drop below 300, see
  [Notes](#notes).

## Fast requests

Columns are `requests / batch_size`.

| Variant | Run | Requests | batch_size | time_taken | Successes | Failures | Worker CPU s | Max RSS MB |
|---|---|---|---|---|---|---|---|---|
| pg_net | 1 | 10000 | 200 | 50.210s | 10000 | 0 | 0.87 | 24 |
| pg_net | 2 | 10000 | 200 | 50.239s | 10000 | 0 | 0.94 | 24 |
| pg_http | 1 | 10000 | 200 | 0.741s | 10000 | 0 | 0.59 | 38 |
| pg_http | 2 | 10000 | 200 | 0.750s | 10000 | 0 | 0.62 | 39 |
| pg_net | 1 | 20000 | 400 | 51.277s | 20000 | 0 | 1.76 | 30 |
| pg_net | 2 | 20000 | 400 | 51.372s | 20000 | 0 | 1.77 | 30 |
| pg_http | 1 | 20000 | 400 | 1.763s | 20000 | 0 | 1.48 | 56 |
| pg_http | 2 | 20000 | 400 | 1.757s | 20000 | 0 | 1.52 | 57 |
| pg_net | 1 | 40000 | 800 | 53.503s | 40000 | 0 | 3.66 | 49 |
| pg_net | 2 | 40000 | 800 | 53.417s | 40000 | 0 | 3.62 | 48 |
| pg_http | 1 | 40000 | 800 | 3.971s | 40000 | 0 | 3.58 | 96 |
| pg_http | 2 | 40000 | 800 | 3.972s | 40000 | 0 | 3.50 | 94 |

### Tuning the new retire logic (fast requests, 40000 requests, batch_size 800 unless noted)

| Setting | time_taken | Successes | Failures | Worker CPU s | Max RSS MB |
|---|---|---|---|---|---|
| `retire_timeout_ms=10` | 3.938s | 40000 | 0 | 3.57 | 95 |
| `retire_timeout_ms=50` | 3.996s | 40000 | 0 | 3.50 | 94 |
| `retire_timeout_ms=100` (default) | 3.951s | 40000 | 0 | 3.42 | 93 |
| `retire_timeout_ms=500` | 3.970s | 40000 | 0 | 3.52 | 94 |
| `retire_batch_size=25` | 3.939s | 40000 | 0 | 3.55 | 95 |
| `retire_batch_size=100` | 4.087s | 40000 | 0 | 3.71 | 97 |
| `retire_batch_size=400` (default, = 800 / 2) | 3.943s | 40000 | 0 | 3.57 | 96 |
| `batch_size=100` | 3.304s | 40000 | 0 | 2.82 | 86 |
| `batch_size=400` | 3.589s | 40000 | 0 | 3.01 | 87 |
| `batch_size=1600` | 5.090s | 40000 | 0 | 4.44 | 106 |

For fast requests the worker is CPU bound in libcurl, so retire settings make no difference, and a
very large `batch_size` is slower.

## Slow requests

A share of the requests reply only after 2 seconds (`SLOW_PCT`, `SLOW_DELAY=2`). Every pg_net
batch contains slow requests, so each batch takes about 2s for the slowest request plus the 1s
sleep.

| Variant | Run | Requests | batch_size | Slow | time_taken | Successes | Failures | Worker CPU s | Max RSS MB |
|---|---|---|---|---|---|---|---|---|---|
| pg_net | 1 | 10000 | 200 | 5% | 149.980s | 10000 | 0 | 0.83 | 23 |
| pg_net | 2 | 10000 | 200 | 5% | 150.078s | 10000 | 0 | 0.93 | 23 |
| pg_http | 1 | 10000 | 200 | 5% | 6.265s | 10000 | 0 | 0.96 | 34 |
| pg_http | 2 | 10000 | 200 | 5% | 6.268s | 10000 | 0 | 0.98 | 34 |
| pg_net | 1 | 10000 | 200 | 1% | 150.018s | 10000 | 0 | 0.81 | 23 |
| pg_net | 2 | 10000 | 200 | 1% | 149.710s | 10000 | 0 | 0.85 | 23 |
| pg_http | 1 | 10000 | 200 | 1% | 2.727s | 10000 | 0 | 0.61 | 34 |
| pg_http | 2 | 10000 | 200 | 1% | 2.698s | 10000 | 0 | 0.58 | 34 |
| pg_net | 1 | 40000 | 800 | 5% | 153.090s | 40000 | 0 | 3.74 | 47 |
| pg_net | 1 | 40000 | 800 | 5% | 156.032s | 40000 | 0 | 3.88 | 47 |
| pg_http | 1 | 40000 | 800 | 5% | 7.084s | 40000 | 0 | 4.07 | 77 |
| pg_http | 2 | 40000 | 800 | 5% | 7.080s | 40000 | 0 | 4.05 | 77 |

### Effect of `batch_size` on the slow mix (pg_http only, 10000 requests, 5% slow)

| batch_size | time_taken | Successes | Failures | Worker CPU s | Max RSS MB |
|---|---|---|---|---|---|
| 200 | 6.265s | 10000 | 0 | 0.96 | 34 |
| 400 | 4.231s | 10000 | 0 | 0.80 | 37 |
| 800 | 2.863s | 10000 | 0 | 0.81 | 43 |

## Reading the results

* With fast requests pg_net takes about `requests / batch_size` seconds (50 batches, 50 to 53s)
  because of the 1s sleep per batch. With 1% or 5% slow requests it takes about 150s, i.e.
  50 batches times (2s slowest request + 1s sleep). pg_http finishes the same work in 0.7s to 7.1s,
  20x to 70x faster, with similar worker CPU and about 1.5x to 2x the memory, because it holds up
  to a retire batch of finished responses in memory.
* pg_http never has more than `batch_size` requests in flight. With 5% slow requests the in-flight
  slots fill up with slow ones: 500 slow requests of 2s on 200 slots need at least 5s, and
  10000 / 200 took 6.27s. A larger `batch_size` removes the limit (2.86s at 800). Tune
  `batch_size` to the concurrency you want.
* The 2s delay puts a floor under any slow run: the last slow request starts after about 0.7s of
  processing, so about 2.7s is the best possible here.

## Notes

* Latency per request was not measured, only the time for the whole run.
* Each result is a single run on a laptop with other processes running. Runs of the same
  configuration agreed within about 1% (a few percent for CPU).
* pg_net's own `net-loadtest` could not run on this machine: xpg writes the worker pid file with
  `grep -oP`, which macOS grep does not support, so `psrecord` got no pid. The same
  `nix/loadtest.sh` script ran against both extensions instead (`EXT=pg_net` for pg_net).
