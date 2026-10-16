#ifndef PG_HTTP_CLIENT_H
#define PG_HTTP_CLIENT_H

#include "pg_prelude.h"

// pragmas needed to pass compiling with -Wextra
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <curl/curl.h>
#include <curl/multi.h>

#pragma GCC diagnostic pop

#include "poller.h"

/*
 * An HTTP client driven by libcurl's multi socket interface. libcurl tells
 * the client which sockets and timeouts it needs via callbacks, which are
 * forwarded to the poller. The owner of the client must wait on `poller->fd`
 * and call http_client_process whenever it becomes readable.
 */
typedef struct HttpClient {
  // The poller which is used to wait for socket and timer events. It is
  // owned by the caller and must outlive the client.
  Poller *poller;

  // The curl multi handle
  CURLM *multi;

  // Number of transfers which are still in progress
  int running;
} HttpClient;

/*
 * Initializes a client which uses `poller` for I/O multiplexing.
 * curl_global_init must have been called before. Returns zero on
 * success and -1 on error.
 */
int http_client_init(HttpClient *client, Poller *poller);

/*
 * Aborts all in-flight requests and releases all resources.
 */
void http_client_close(HttpClient *client);

/*
 * Starts a GET request to `url`. The request makes progress when
 * http_client_process is called. Returns zero on success and -1 on error.
 */
int http_client_get(HttpClient *client, const char *url);

/*
 * Collects ready events from the poller without blocking, advances the
 * transfers accordingly and reports the finished ones. Returns zero on
 * success and -1 on error.
 */
int http_client_process(HttpClient *client);

#endif /* PG_HTTP_CLIENT_H */
