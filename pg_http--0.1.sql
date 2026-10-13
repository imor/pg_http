-- complain if script is sourced in psql, rather than via create extension
\echo Use "create extension pg_http" to load this file. \quit

create type item AS (key text, value text);

create function split_pair(text)
returns item language C as 'MODULE_PATHNAME'