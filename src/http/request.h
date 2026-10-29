#ifndef PG_HTTP_REQUEST_H
#define PG_HTTP_REQUEST_H

#include "pg_prelude.h"

#include <curl/curl.h>
#include <stddef.h>
#include <stdint.h>

/*
 * An HTTP request. Contains everything needed to initiate
 * the request.
 */
typedef struct HttpRequest {
  // Holds the memory of the request, except the headers. Owned by the request, until a transfer
  // takes it over.
  MemoryContext context;

  // The id of the request in the queue table.
  int64_t id;

  // The method of the request: GET, POST or DELETE (any case).
  const char *method;

  // The URL to which to send the request.
  const char *url;

  // The maximum time the request may take, in milliseconds.
  int32_t timeout_milliseconds;

  // The request headers, each one in the `Name: value` format. NULL if there are none. libcurl
  // doesn't copy the list, so it must stay alive until the transfer is done. It is malloc'd by
  // libcurl and owned by the request, until a transfer takes it over.
  struct curl_slist *headers;

  // The request body, NULL if there is none.
  const char *body;

  // Length of the body in bytes
  size_t      body_len;
} HttpRequest;

/*
 * Returns NULL if the request is valid and can be sent, otherwise a palloc'd
 * message describing why it can't be sent. `max_timeout_ms` is the upper bound
 * for the timeout of a request.
 */
char *http_request_rejection(const HttpRequest *request, int max_timeout_ms);

#endif /* PG_HTTP_REQUEST_H */
