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
* Throughput is `requests / time_taken`, rounded to whole requests per second.
* `time_taken` is the time from the end of the inserts until all N responses are in the
  responses table. CPU is the worker's cumulative CPU time, RSS its maximum resident size
  (sampled every 200 ms, so a peak can be missed).
* The loadtest waits for all N responses (`count(*) = N`), not for the last request id.
* Between runs the script waits for sockets in `TIME_WAIT` to drop below 300, see
  [Notes](#notes).

## Fast requests

Columns are `requests / batch_size`.

| Variant | Run | Requests | batch_size | time_taken | Throughput (req/s) | Successes | Failures | Worker CPU s | Max RSS MB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| pg_net | 1 | 10000 | 200 | 50.210s | 199 | 10000 | 0 | 0.87 | 24 |
| pg_net | 2 | 10000 | 200 | 50.239s | 199 | 10000 | 0 | 0.94 | 24 |
| pg_http | 1 | 10000 | 200 | 0.741s | 13,495 | 10000 | 0 | 0.59 | 38 |
| pg_http | 2 | 10000 | 200 | 0.750s | 13,333 | 10000 | 0 | 0.62 | 39 |
| pg_net | 1 | 20000 | 400 | 51.277s | 390 | 20000 | 0 | 1.76 | 30 |
| pg_net | 2 | 20000 | 400 | 51.372s | 389 | 20000 | 0 | 1.77 | 30 |
| pg_http | 1 | 20000 | 400 | 1.763s | 11,344 | 20000 | 0 | 1.48 | 56 |
| pg_http | 2 | 20000 | 400 | 1.757s | 11,383 | 20000 | 0 | 1.52 | 57 |
| pg_net | 1 | 40000 | 800 | 53.503s | 748 | 40000 | 0 | 3.66 | 49 |
| pg_net | 2 | 40000 | 800 | 53.417s | 749 | 40000 | 0 | 3.62 | 48 |
| pg_http | 1 | 40000 | 800 | 3.971s | 10,073 | 40000 | 0 | 3.58 | 96 |
| pg_http | 2 | 40000 | 800 | 3.972s | 10,070 | 40000 | 0 | 3.50 | 94 |

### Tuning the new retire logic (fast requests, 40000 requests, batch_size 800 unless noted)

| Setting | time_taken | Throughput (req/s) | Successes | Failures | Worker CPU s | Max RSS MB |
| --- | --- | --- | --- | --- | --- | --- |
| `retire_timeout_ms=0` (retire as soon as anything is pending) | 4.176s | 9,578 | 40000 | 0 | 3.74 | 96 |
| `retire_timeout_ms=10` | 4.064s | 9,844 | 40000 | 0 | 3.62 | 96 |
| `retire_timeout_ms=50` | 4.167s | 9,598 | 40000 | 0 | 3.53 | 93 |
| `retire_timeout_ms=100` (default) | 4.044s | 9,892 | 40000 | 0 | 3.46 | 93 |
| `retire_timeout_ms=500` | 4.066s | 9,837 | 40000 | 0 | 3.45 | 92 |
| `retire_batch_size=25` | 4.062s | 9,847 | 40000 | 0 | 3.46 | 93 |
| `retire_batch_size=100` | 4.077s | 9,812 | 40000 | 0 | 3.69 | 97 |
| `retire_batch_size=400` (default, = 800 / 2) | 4.069s | 9,832 | 40000 | 0 | 3.47 | 93 |
| `batch_size=100` | 3.422s | 11,688 | 40000 | 0 | 2.76 | 83 |
| `batch_size=400` | 3.673s | 10,890 | 40000 | 0 | 3.16 | 88 |
| `batch_size=1600` | 4.973s | 8,044 | 40000 | 0 | 4.49 | 106 |

For fast requests the worker is CPU bound in libcurl, so the retire settings make little
difference: the runs agree within about 3% in time, which is run to run noise. The only visible
effect is a few percent more CPU with `retire_timeout_ms=0` and `retire_batch_size=100`. A very
large `batch_size` is slower, a small one a bit faster.

## Slow requests

A share of the requests reply only after 2 seconds (`SLOW_PCT`, `SLOW_DELAY=2`). Every pg_net
batch contains slow requests, so each batch takes about 2s for the slowest request plus the 1s
sleep.

| Variant | Run | Requests | batch_size | Slow | time_taken | Throughput (req/s) | Successes | Failures | Worker CPU s | Max RSS MB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| pg_net | 1 | 10000 | 200 | 5% | 149.980s | 67 | 10000 | 0 | 0.83 | 23 |
| pg_net | 2 | 10000 | 200 | 5% | 150.078s | 67 | 10000 | 0 | 0.93 | 23 |
| pg_http | 1 | 10000 | 200 | 5% | 6.265s | 1,596 | 10000 | 0 | 0.96 | 34 |
| pg_http | 2 | 10000 | 200 | 5% | 6.268s | 1,595 | 10000 | 0 | 0.98 | 34 |
| pg_net | 1 | 10000 | 200 | 1% | 150.018s | 67 | 10000 | 0 | 0.81 | 23 |
| pg_net | 2 | 10000 | 200 | 1% | 149.710s | 67 | 10000 | 0 | 0.85 | 23 |
| pg_http | 1 | 10000 | 200 | 1% | 2.727s | 3,667 | 10000 | 0 | 0.61 | 34 |
| pg_http | 2 | 10000 | 200 | 1% | 2.698s | 3,706 | 10000 | 0 | 0.58 | 34 |
| pg_net | 1 | 40000 | 800 | 5% | 153.090s | 261 | 40000 | 0 | 3.74 | 47 |
| pg_net | 1 | 40000 | 800 | 5% | 156.032s | 256 | 40000 | 0 | 3.88 | 47 |
| pg_http | 1 | 40000 | 800 | 5% | 7.084s | 5,647 | 40000 | 0 | 4.07 | 77 |
| pg_http | 2 | 40000 | 800 | 5% | 7.080s | 5,650 | 40000 | 0 | 4.05 | 77 |

### Effect of `batch_size` on the slow mix (pg_http only, 10000 requests, 5% slow)

| batch_size | time_taken | Throughput (req/s) | Successes | Failures | Worker CPU s | Max RSS MB |
| --- | --- | --- | --- | --- | --- | --- |
| 200 | 6.265s | 1,596 | 10000 | 0 | 0.96 | 34 |
| 400 | 4.231s | 2,364 | 10000 | 0 | 0.80 | 37 |
| 800 | 2.863s | 3,493 | 10000 | 0 | 0.81 | 43 |

## Comparison with pg_rest

pg_rest (`../../rust/pg_rest`, Rust with tokio and reqwest) run through the same `nix/loadtest.sh`
(extended to know pg_rest's `rest.*` tables), against the same nginx and PG 17.0, single runs.
pg_rest was built with `cargo pgrx package` against that PG. It has no `batch_size`: it always
allows 1000 requests in flight, commits responses in buckets of 100 or after 50 ms, and uses 2
tokio worker threads.

### Fast requests

| Variant | Requests | batch_size | time_taken | Throughput (req/s) | Worker CPU s | Max RSS MB |
| --- | --- | --- | --- | --- | --- | --- |
| pg_http | 10000 | 200 | 0.769s | 13,001 | 0.64 | 39 |
| pg_rest | 10000 | n/a | 0.285s | 35,085 | 0.46 | 54 |
| pg_http | 20000 | 400 | 1.787s | 11,192 | 1.45 | 56 |
| pg_rest | 20000 | n/a | 0.444s | 45,000 | 1.37 | 90 |
| pg_http | 40000 | 800 | 4.111s | 9,730 | 3.71 | 97 |
| pg_rest | 40000 | n/a | 1.110s | 36,036 | 3.25 | 144 |
| pg_http | 40000 | 1000 | 4.347s | 9,201 | 3.84 | 98 |

### Slow requests (10000 requests, 2s delay, pg_http `batch_size=1000`)

| Slow share | Variant | time_taken | Worker CPU s | Max RSS MB |
| --- | --- | --- | --- | --- |
| 1% | pg_http | 3.092s | 1.01 | 46 |
| 1% | pg_rest | 2.245s | 1.32 | 111 |
| 5% | pg_http | 2.999s | 0.95 | 46 |
| 5% | pg_rest | 2.249s | 2.44 | 184 |

* pg_rest is 2.7x to 3.7x faster on fast requests, also against pg_http at `batch_size=1000`,
  so the in-flight limit does not explain the gap.
* CPU time is similar (pg_rest 5% to 30% lower on fast requests). pg_rest peaks at about 1.4x to
  1.5x the RSS of pg_http.
* With slow requests pg_rest finishes in about one slow-request time, pg_http about 0.8s later.
  pg_rest's CPU and RSS grow with the slow share (2.6x the CPU and 4x the RSS of pg_http at 5%).
  Neither was investigated.

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
