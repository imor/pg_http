create database pre_existing;
create role pre_existing nosuperuser login;

\c postgres
create extension pg_http;
\ir ./utils/loadtest.sql
\ir ./utils/helpers.sql
