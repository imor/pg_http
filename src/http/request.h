#ifndef PG_HTTP_REQUEST_H
#define PG_HTTP_REQUEST_H

#include <stdint.h>

/*
 * An HTTP request. Contains everything needed to initiate
 * the request.
 */
typedef struct HttpRequest {
  // The id of the request in the queue table.
  int64_t id;

  // The URL to which to send the request.
  const char *url;
} HttpRequest;

#endif /* PG_HTTP_REQUEST_H */
