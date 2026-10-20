#ifndef PG_HTTP_CLIENT_H
#define PG_HTTP_CLIENT_H

#include "poller/poller.h"
#include "request.h"

#include <curl/curl.h>
#include <curl/multi.h>

/*
 * An HTTP client that uses libcurl and the Poller to drive
 * the I/O for sending requests and receiving them.
 */
typedef struct HttpClient {
  // Poller used for I/O and timer events
  Poller poller;

  CURLM *multi_handle;
} HttpClient;

/*
 * Initialize the `HttpClient`.
 */
int http_client_init(HttpClient *client);

/*
 * Send an HTTP request
 */
int http_send(HttpClient *client, Request *request);

/*
 * Close the `HttpClient`
 */
int http_client_close(HttpClient *client);

#endif /* PG_HTTP_CLIENT_H */
