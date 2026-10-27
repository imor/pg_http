-- complain if script is sourced in psql, rather than via create extension
\echo Use "create extension pg_http" to load this file. \quit

create schema if not exists http;

create type http.request_status as enum ('queued', 'in_flight', 'completed');

create domain http.http_method as text
check (
  value ilike 'get'
  or value ilike 'post'
  or value ilike 'delete'
);

-- Pending and in-flight requests, the background worker reads from here
-- API: Private
create table http.requests(
    id bigint generated always as identity primary key,
    method http.http_method not null,
    url text not null,
    headers jsonb,
    body bytea,
    timeout_milliseconds int not null,
    status http.request_status not null default 'queued'
);

create index on http.requests (id) where status = 'queued';

create type http.error_type as enum ('timeout', 'connection_error', 'other');

-- Responses for completed requests, `id` is the id of the request
create table http.responses(
    id bigint primary key,
    status_code integer,
    headers jsonb,
    body text,
    error_type http.error_type,
    error_msg text,
    created timestamptz not null default now()
);

create index on http.responses (created);

-- url encode a string
-- API: Private
create function http._urlencode_string(string varchar)
    returns text
    language 'c'
    immutable
as 'MODULE_PATHNAME';

-- API: Private
create function http._encode_url_with_params_array(url text, params_array text[])
    returns text
    language 'c'
    immutable
as 'MODULE_PATHNAME';

-- Restarts the background worker
create function http.worker_restart()
    returns bool
    language 'c'
as 'MODULE_PATHNAME';

-- Blocks until the background worker is running
create function http.wait_until_running()
    returns void
    language 'c'
as 'MODULE_PATHNAME';
comment on function http.wait_until_running() is 'waits until the worker is running';

-- Wakes the background worker when the current transaction commits
create function http.wake()
    returns void
    language 'c'
as 'MODULE_PATHNAME';

create function http.check_worker_is_up() returns void as $$
begin
  if not exists (select pid from pg_stat_activity where backend_type ilike '%pg_http%') then
    raise exception using
      message = 'the pg_http background worker is not up'
    , detail  = 'the pg_http background worker is down due to an internal error and cannot process requests'
    , hint    = 'make sure that you didn''t modify any of pg_http internal tables';
  end if;
end
$$ language plpgsql;
comment on function http.check_worker_is_up() is 'raises an exception if the pg_http background worker is not up, otherwise it doesn''t return anything';

-- Interface to make an async GET request
-- API: Public
create function http.http_get(
    -- url for the request
    url text,
    -- key/value pairs to be url encoded and appended to the `url`
    params jsonb default '{}'::jsonb,
    -- key/values to be included in request headers
    headers jsonb default '{}'::jsonb,
    -- the maximum number of milliseconds the request may take before being cancelled
    timeout_milliseconds int default 5000
)
    -- request_id reference
    returns bigint
    language plpgsql
as $$
declare
    request_id bigint;
    params_array text[];
begin
    select coalesce(array_agg(http._urlencode_string(key) || '=' || http._urlencode_string(value)), '{}')
    into params_array
    from jsonb_each_text(params);

    insert into http.requests(method, url, headers, timeout_milliseconds)
    values (
        'GET',
        http._encode_url_with_params_array(url, params_array),
        headers,
        timeout_milliseconds
    )
    returning id
    into request_id;

    perform http.wake();

    return request_id;
end
$$;

-- Interface to make an async POST request
-- API: Public
create function http.http_post(
    -- url for the request
    url text,
    -- body of the POST request
    body jsonb default '{}'::jsonb,
    -- key/value pairs to be url encoded and appended to the `url`
    params jsonb default '{}'::jsonb,
    -- key/values to be included in request headers
    headers jsonb default '{"Content-Type": "application/json"}'::jsonb,
    -- the maximum number of milliseconds the request may take before being cancelled
    timeout_milliseconds int default 5000
)
    -- request_id reference
    returns bigint
    language plpgsql
as $$
declare
    request_id bigint;
    params_array text[];
    content_type text;
begin
    -- Extract the content_type from headers
    select header_value into content_type
    from jsonb_each_text(coalesce(headers, '{}'::jsonb)) r(header_name, header_value)
    where lower(header_name) = 'content-type'
    limit 1;

    -- If the user provided new headers and omitted the content type
    -- add it back in automatically
    if content_type is null then
        select headers || '{"Content-Type": "application/json"}'::jsonb into headers;
    end if;

    -- Confirm that the content-type is set as "application/json"
    if content_type <> 'application/json' then
        raise exception 'Content-Type header must be "application/json"';
    end if;

    select coalesce(array_agg(http._urlencode_string(key) || '=' || http._urlencode_string(value)), '{}')
    into params_array
    from jsonb_each_text(params);

    insert into http.requests(method, url, headers, body, timeout_milliseconds)
    values (
        'POST',
        http._encode_url_with_params_array(url, params_array),
        headers,
        convert_to(body::text, 'UTF8'),
        timeout_milliseconds
    )
    returning id
    into request_id;

    perform http.wake();

    return request_id;
end
$$;

-- Interface to make an async DELETE request
-- API: Public
create function http.http_delete(
    -- url for the request
    url text,
    -- key/value pairs to be url encoded and appended to the `url`
    params jsonb default '{}'::jsonb,
    -- key/values to be included in request headers
    headers jsonb default '{}'::jsonb,
    -- the maximum number of milliseconds the request may take before being cancelled
    timeout_milliseconds int default 5000,
    -- optional body of the request
    body jsonb default NULL
)
    -- request_id reference
    returns bigint
    language plpgsql
as $$
declare
    request_id bigint;
    params_array text[];
begin
    select coalesce(array_agg(http._urlencode_string(key) || '=' || http._urlencode_string(value)), '{}')
    into params_array
    from jsonb_each_text(params);

    insert into http.requests(method, url, headers, body, timeout_milliseconds)
    values (
        'DELETE',
        http._encode_url_with_params_array(url, params_array),
        headers,
        convert_to(body::text, 'UTF8'),
        timeout_milliseconds
    )
    returning id
    into request_id;

    perform http.wake();

    return request_id;
end
$$;

-- Outcome of collecting the response of a request
-- API: Public
create type http.result_status as enum ('PENDING', 'SUCCESS', 'ERROR');

-- A response from an HTTP server
-- API: Public
create type http.http_response as (
    status_code integer,
    headers jsonb,
    body text
);

-- State wrapper around responses
-- API: Public
create type http.http_response_result as (
    status http.result_status,
    message text,
    response http.http_response
);

-- Blocks until a request has a response
-- API: Private
create function http._await_response(request_id bigint)
    returns bool
    language plpgsql
as $$
declare
    rec http.responses;
begin
    while rec is null loop
        select * into rec from http.responses where id = request_id;

        if rec is null then
            -- Wait 50 ms before checking again
            perform pg_sleep(0.05);
        end if;
    end loop;

    return true;
end;
$$;

-- Collect the response of an http request
-- API: Private
create function http._collect_response(
    -- request_id reference
    request_id bigint,
    -- when `true`, return immediately. when `false` wait for the request to complete before returning
    async bool default true
)
    returns http.http_response_result
    language plpgsql
as $$
declare
    rec http.responses;
begin
    if not async then
        perform http._await_response(request_id);
    end if;

    select * into rec from http.responses where id = request_id;

    if rec is null or rec.error_msg is not null then
        -- The request is either still processing or the request_id provided does not exist
        return (
            'ERROR',
            coalesce(rec.error_msg, 'request matching request_id not found'),
            null
        )::http.http_response_result;
    end if;

    return (
        'SUCCESS',
        'ok',
        (rec.status_code, rec.headers, rec.body)::http.http_response
    )::http.http_response_result;
end;
$$;

grant usage on schema http to PUBLIC;
grant all on all sequences in schema http to PUBLIC;
grant all on all tables in schema http to PUBLIC;
