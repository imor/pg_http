create table run (
  requests int,
  batch_size int,
  time_taken interval,
  request_successes bigint,
  request_failures bigint,
  last_failure_error text
);

-- loadtest using many gets, used to be called `repro_timeouts`
create or replace procedure wait_for_many_gets(number_of_requests int default 10000, url text default 'http://localhost:8080') as $$
declare
  first_time timestamptz;
  second_time timestamptz;

  request_successes bigint;
  request_failures bigint;
  last_failure_error text;
begin
  delete from http.requests;
  delete from http.responses;

  with do_requests as (
    select
      http.http_get(url) as id
    from generate_series (1, number_of_requests) x
  )
  select clock_timestamp() into first_time from do_requests offset number_of_requests - 1;

  commit;

  raise notice 'Waiting until % requests complete, using a pg_http.batch_size of %', number_of_requests, current_setting('pg_http.batch_size')::text;

  -- wait for all the responses, not just the last one, responses are not retired in request order
  while (select count(*) from http.responses) < number_of_requests loop
    perform pg_sleep(0.01);
  end loop;

  select clock_timestamp() into second_time;

  select
    count(*) filter (where error_msg is null),
    count(*) filter (where error_msg is not null),
    (select error_msg from http.responses where error_msg is not null order by id desc limit 1)
  into request_successes, request_failures, last_failure_error
  from http.responses;

  insert into run values (
    number_of_requests, current_setting('pg_http.batch_size')::int, age(second_time, first_time),
    request_successes, request_failures, last_failure_error);
end;
$$ language plpgsql;
