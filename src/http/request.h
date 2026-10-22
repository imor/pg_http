#ifndef PG_HTTP_REQUEST_H
#define PG_HTTP_REQUEST_H

/*
 * An HTTP request. Contains everything needed to initiate
 * the request.
 */
typedef struct HttpRequest {
  // The URL to which to send the request.
  const char *url;
} HttpRequest;

#endif /* PG_HTTP_REQUEST_H */
