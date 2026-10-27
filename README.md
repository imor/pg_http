# pg_http

## How pg_http differs from pg_net

Both extensions queue requests in a table and send them from a background worker using libcurl's
multi interface. They differ in how the worker batches work and when it talks to the database.

|                          | pg_net                                                                                                        | pg_http                                                                                                                           |
| ------------------------ | ------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------- |
| Unit of work             | A batch of `batch_size` requests. The next batch starts only after every request of the current one finished. | A sliding window: up to `batch_size` requests in flight, replaced as responses are retired.                                       |
| Pause between work       | Always sleeps 1 second after a batch.                                                                         | None. The worker sleeps only when it has nothing to do.                                                                           |
| Transaction              | One transaction per batch, open for the whole time the requests are on the network.                           | Short transactions that only read or write tables. No transaction is open during network I/O.                                     |
| Taking requests          | `DELETE ... RETURNING` from the queue inside the batch transaction.                                           | `UPDATE ... SET status = 'in_flight'`, committed right away.                                                                      |
| Saving responses         | One `INSERT` per response, inside the batch transaction.                                                      | Responses are kept in memory and saved with one bulk `INSERT` (`unnest` of arrays) plus one `UPDATE` of the request statuses.     |
| When responses are saved | When the whole batch is done.                                                                                 | When `retire_batch_size` responses accumulated, `retire_timeout_ms` passed since the oldest one, or nothing is in flight anymore. |
| Refilling                | Next batch, after the sleep.                                                                                  | In the same transaction that retires responses, up to `batch_size - in flight`.                                                   |
| Completed rows           | Request row is deleted when taken.                                                                            | Request row stays as `completed` until `pg_http.ttl` expires it together with its response.                                       |

### What makes pg_http faster

- **No batch barrier.** In pg_net a batch lasts as long as its slowest request, and every batch
  contains slow requests when a small share of requests is slow. One slow request leaves the rest
  of the batch's capacity idle. pg_http replaces finished requests continuously, so a slow request
  occupies one slot and the other slots keep working.
- **No fixed sleep.** pg_net's 1s sleep after each batch puts a floor of about one second per
  `batch_size` requests under its throughput. With fast requests pg_net takes about
  `requests / batch_size` seconds whatever the hardware. pg_http has no such floor, so with fast
  requests it is limited by curl and CPU instead.
- **Short transactions.** pg_net holds a snapshot and locks on the extension tables for as long
  as the slowest request of the batch takes. pg_http only holds a transaction while it reads or
  writes tables, so it doesn't hold back vacuum or conflicting DDL while waiting on the network.
- **Fewer round trips to the tables.** Retiring a batch of responses and refilling take one
  transaction and a few statements instead of an insert per response, and wakes while the queue
  is known to be empty don't query it.

### What it costs

- **Memory.** Finished responses (headers and body) stay in the worker's memory until they are
  retired, up to roughly `retire_batch_size` of them, on top of the requests in flight. In the
  benchmarks the worker used about 1.5x to 2x the memory of pg_net's (for example 77 MB against
  47 MB at `batch_size=800`). Lower `batch_size`, `retire_batch_size`, or `retire_timeout_ms` to trade throughput
  for memory.

CPU use is about the same in both because it is dominated by libcurl. See
[Benchmarks](#benchmarks) and [BENCHMARKS.md](BENCHMARKS.md) for measurements.

## Benchmarks

The load test starts a temporary database and an nginx server, makes the requests, waits for all
of the responses, and prints a results table plus the worker's CPU time and memory. Results
and the settings they ran at are in [BENCHMARKS.md](BENCHMARKS.md).

Build once, then run `http-loadtest` from the dev shell:

```sh
nix develop -c xpg -v 17 build

# http-loadtest REQUESTS BATCH_SIZE [-- extra postgres options]
nix develop -c http-loadtest 10000 200
nix develop -c http-loadtest 40000 800
```

`BATCH_SIZE` sets `pg_http.batch_size`, the maximum number of requests in flight. Other settings can
be passed after `--`:

```sh
nix develop -c http-loadtest 40000 800 -- -c pg_http.retire_timeout_ms=50 -c pg_http.retire_batch_size=100
```

### Slow requests

Set `SLOW_PCT` to make that percentage of the requests reply after `SLOW_DELAY` seconds
(default 2). The rest reply immediately.

```sh
SLOW_PCT=5 SLOW_DELAY=2 nix develop -c http-loadtest 10000 200
```

### Comparing with pg_net

Run the same script from the pg_net checkout, with `EXT=pg_net`:

```sh
cd ../pg_net
nix develop -c xpg -v 17 build
EXT=pg_net nix develop -c bash ../pg_http/nix/loadtest.sh 10000 200
EXT=pg_net SLOW_PCT=5 SLOW_DELAY=2 nix develop -c bash ../pg_http/nix/loadtest.sh 10000 200
```

pg_net sleeps one second after every batch, so its slow-request runs take around 150 seconds.

### Notes

- Wait between runs until `netstat -an | grep -c TIME_WAIT` is below about 300. Back to back runs
  use up the ephemeral ports and show up as `Could not connect to server` failures.
- Check `request_failures` in the results table first if your numbers differ from
  [BENCHMARKS.md](BENCHMARKS.md).
- The raw results and CPU samples of a run are left in `test/load/`, which is gitignored.
- Nix only sees files tracked by git. If `nix develop` can't find `nix/loadtest.nix` or the nginx
  config, run `git add -N nix`.
