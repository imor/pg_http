set -euo pipefail

# Usage: [SLOW_PCT=5 SLOW_DELAY=2] http-loadtest [REQUESTS] [BATCH_SIZE] [-- extra postgres options, e.g. -c pg_http.retire_timeout_ms=50]
#
# With SLOW_PCT set, that percentage of the requests (spread evenly through the batch) goes to an
# endpoint which replies after SLOW_DELAY seconds (default 2), the rest reply immediately.
#
# Starts a temporary db and an nginx server, waits until REQUESTS requests are done, then reports
# the results plus the CPU time and memory of the background worker. EXT selects the extension
# whose GUCs and worker are used, it defaults to pg_http.

ext=${EXT:-pg_http}
reqs=${1:-10000}
batch=${2:-200}
shift $(($# > 2 ? 2 : $#)) || true
[ "${1:-}" = "--" ] && shift
extra_opts="$*"

load_dir=test/load
mkdir -p "$load_dir"
echo "*" > "$load_dir/.gitignore"
query_csv=$load_dir/query.csv
sample_log=$load_dir/worker.log
rm -f "$query_csv" "$sample_log"

# cputime is formatted as [hh:]mm:ss.cc
cpu_seconds() {
  echo "$1" | awk -F'[:]' '{ s = 0; for (i = 1; i <= NF; i++) s = s * 60 + $i; printf "%.2f", s }'
}

slow_pct=${SLOW_PCT:-}
slow_delay=${SLOW_DELAY:-2}
if [ "$ext" = pg_net ]; then
  schema=net resp=net._http_response clear="delete from net._http_response"
else
  schema=http resp=http.responses clear="delete from http.requests; delete from http.responses"
fi

call="call wait_for_many_gets($reqs)"
define="select 1"
if [ -n "$slow_pct" ]; then
  call="call wait_for_mixed_gets($reqs, $slow_pct, $slow_delay)"
  define="create or replace procedure wait_for_mixed_gets(n int, slow_pct int, slow_delay int) as \$\$
    declare first_time timestamptz; second_time timestamptz; ok bigint; bad bigint; last_err text;
    begin
      $clear;
      perform $schema.http_get(case when x % 100 < slow_pct
        then 'http://localhost:8080/pathological?status=200&delay=' || slow_delay
        else 'http://localhost:8080/pathological?status=200' end) from generate_series(1, n) x;
      select clock_timestamp() into first_time;
      commit;
      while (select count(*) from $resp) < n loop perform pg_sleep(0.01); end loop;
      select clock_timestamp() into second_time;
      select count(*) filter (where error_msg is null), count(*) filter (where error_msg is not null),
        (select error_msg from $resp where error_msg is not null order by id desc limit 1)
      into ok, bad, last_err from $resp;
      insert into run values (n, current_setting('$ext.batch_size')::int, age(second_time, first_time), ok, bad, last_err);
    end; \$\$ language plpgsql"
fi

net-with-nginx xpg --options "-c log_min_messages=WARNING -c $ext.batch_size=$batch $extra_opts" \
  psql -c "$define" -c "$call" -c "\pset format csv" -c "\o $query_csv" -c "select * from run" > /dev/null 2>&1 &
runner=$!

# sample the worker until the run is over, its pid is not known up front
max_rss=0
cpu=0
while kill -0 "$runner" 2> /dev/null; do
  pid=$(pgrep -f "postgres: ($ext|.* $ext) .*worker" | head -1 || true)
  if [ -n "$pid" ]; then
    read -r rss cputime <<< "$(ps -o rss=,cputime= -p "$pid" 2> /dev/null || true)" || true
    if [ -n "${rss:-}" ]; then
      [ "$rss" -gt "$max_rss" ] && max_rss=$rss
      cpu=$(cpu_seconds "$cputime")
      echo "$(date +%s.%N 2> /dev/null || date +%s) $cputime $rss" >> "$sample_log"
    fi
  fi
  sleep 0.2
done
wait "$runner" || true

echo -e "## Loadtest results ($ext, batch_size=$batch${extra_opts:+, $extra_opts}${slow_pct:+, ${slow_pct}% of requests slow by ${slow_delay}s})\n"
if [ -s "$query_csv" ]; then
  awk -F, 'NR == 1 { print "| " $1 " | " $2 " | " $3 " | " $4 " | " $5 " | " $6 " |"; print "|---|---|---|---|---|---|"; next }
           { print "| " $1 " | " $2 " | " $3 " | " $4 " | " $5 " | " $6 " |" }' "$query_csv"
else
  echo "no results, the loadtest failed"
fi
echo -e "\nworker cpu seconds: $cpu, max rss: $((max_rss / 1024)) MB"
