#ifndef PG_HTTP_REQUEST_H
#define PG_HTTP_REQUEST_H

#include <stddef.h>
#include <stdint.h>

/*
 * An HTTP request. Contains everything needed to initiate
 * the request.
 */
typedef struct HttpRequest {
  // The id of the request in the queue table.
  int64_t id;

  // The method of the request: GET, POST or DELETE (any case).
  const char *method;

  // The URL to which to send the request.
  const char *url;

  // The maximum time the request may take, in milliseconds.
  int32_t timeout_milliseconds;

  // The request headers, each one in the `Name: value` format. NULL if there are none.
  const char *const *headers;
  int                num_headers;

  // The request body, NULL if there is none.
  const char *body;
  size_t      body_len;
} HttpRequest;

/*
 * Returns NULL if the request is valid and can be sent, otherwise a palloc'd
 * message describing why it can't be sent. `max_timeout_ms` is the upper bound
 * for the timeout of a request.
 */
char *http_request_rejection(const HttpRequest *request, int max_timeout_ms);

#endif /* PG_HTTP_REQUEST_H */
