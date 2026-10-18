#ifndef PG_HTTP_CLIENT_H
#define PG_HTTP_CLIENT_H

#include "pg_prelude.h"

// pragmas needed to pass compiling with -Wextra
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <curl/curl.h>
#include <curl/multi.h>

#pragma GCC diagnostic pop

#include "poller/poller.h"
#include "response.h"

/*
 * An HTTP client driven by libcurl's multi socket interface. libcurl tells
 * the client which sockets and timeouts it needs via callbacks, which are
 * forwarded to the poller. The owner of the client must wait on `poller.fd`
 * and call http_client_process whenever it becomes readable.
 */
typedef struct HttpClient {
  // The poller which is used to wait for socket and timer events
  Poller poller;

  // The curl multi handle
  CURLM *multi;

  // Number of transfers which are still in progress
  int running;

  // The transfer whose response was last returned by http_client_next
  struct Transfer *finished;
} HttpClient;

/*
 * Initializes a client and creates its poller. Returns zero on success and
 * -1 on error, in which case errno is set if the poller could not be created.
 */
int http_client_init(HttpClient *client);

/*
 * Returns the fd which should be waited to become readable before calling
 * `http_client_process`.
 */
int http_client_fd(HttpClient *client);

/*
 * Starts `req`. The request borrows all its memory (see request.h) and makes
 * progress when http_client_process is called. Returns zero on success and -1
 * on error.
 */
int http_client_send(HttpClient *client, const HttpRequest *req);

/*
 * Returns the next finished response, successfully or not, or NULL if none is
 * ready. The response belongs to the client and is valid until the next call
 * to http_client_next or http_client_close. Call repeatedly after
 * http_client_process until it returns NULL.
 */
const HttpResponse *http_client_next(HttpClient *client);

/*
 * Collects ready events from the poller without blocking, advances the
 * transfers accordingly. Finished ones are retrieved with http_client_next. Returns zero on
 * success and -1 on error.
 */
int http_client_process(HttpClient *client);

/*
 * Aborts all in-flight requests and releases all resources.
 */
void http_client_close(HttpClient *client);

#endif /* PG_HTTP_CLIENT_H */
