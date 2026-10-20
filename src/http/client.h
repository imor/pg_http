#ifndef PG_HTTP_CLIENT_H
#define PG_HTTP_CLIENT_H

#include "poller/poller.h"
#include "request.h"

#include <curl/curl.h>
#include <curl/multi.h>

typedef enum {
  HTTP_CLIENT_OK           = 0,
  HTTP_CLIENT_CURL_ERROR   = -1,
  HTTP_CLIENT_POLLER_ERROR = -2,
} HttpClientStatus;
/*
 * An HTTP client that uses libcurl and the Poller to drive
 * the I/O for sending requests and receiving them.
 */
typedef struct HttpClient HttpClient;

/*
 * Initialize the `HttpClient`.
 */
HttpClientStatus http_client_init(HttpClient *client);

/*
 * Send an HTTP request
 */
HttpClientStatus http_send(HttpClient *client, Request *request);

/*
 * Close the `HttpClient`
 */
HttpClientStatus http_client_close(HttpClient *client);

#endif /* PG_HTTP_CLIENT_H */
