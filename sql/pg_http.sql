-- complain if script is sourced in psql, rather than via create extension
\echo Use "create extension pg_http" to load this file. \quit

create schema if not exists http;

create table http.requests(
    id bigint generated always as identity primary key,
    url text not null
);
