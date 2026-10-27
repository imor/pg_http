# pg_http

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

* Wait between runs until `netstat -an | grep -c TIME_WAIT` is below about 300. Back to back runs
  use up the ephemeral ports and show up as `Could not connect to server` failures.
* Check `request_failures` in the results table first if your numbers differ from
  [BENCHMARKS.md](BENCHMARKS.md).
* The raw results and CPU samples of a run are left in `test/load/`, which is gitignored.
* Nix only sees files tracked by git. If `nix develop` can't find `nix/loadtest.nix` or the nginx
  config, run `git add -N nix`.
