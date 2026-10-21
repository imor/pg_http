#ifndef PG_HTTP_CLIENT_H
#define PG_HTTP_CLIENT_H

#include "poller/poller.h"
#include "request.h"
#include "response.h"

#include <curl/curl.h>
#include <curl/multi.h>

typedef enum {
  HTTP_CLIENT_OK             = 0,
  HTTP_CLIENT_CURL_ERROR     = -1,
  HTTP_CLIENT_POLLER_ERROR   = -2,
  HTTP_CLIENT_RESPONSE_ERROR = -3,
} HttpClientStatus;
/*
 * An HTTP client that uses libcurl and the Poller to drive
 * the I/O for sending requests and receiving them.
 */
typedef struct HttpClient {
  // Poller used for I/O and timer events
  Poller poller;

  // Curl's multi handle
  CURLM *multi;

  // Number of curl transfers still in progress
  int running_handles;
} HttpClient;

/*
 * Initialize the `HttpClient`.
 */
HttpClientStatus http_client_init(HttpClient *client);

/*
 * Send an HTTP request
 */
HttpClientStatus http_client_send(HttpClient *client, HttpRequest *request, HttpResponse *response);

/*
 * This function performs the actual I/O required to send/receive
 * http requests.
 */
HttpClientStatus http_client_process(HttpClient *client);

/*
 * Type of function callback used to receive completed responses
 */
typedef void (*response_callback)(CURLcode code, HttpResponse *response, long status_code);

/*
 * Get responses which have completed in the response callback
 */
HttpClientStatus http_client_get_responses(HttpClient *client, response_callback);

/*
 * Returns the fd which should be waited to become readable before calling
 * `http_client_process`.
 */
int http_client_fd(HttpClient *client);

/*
 * Close the `HttpClient`
 */
HttpClientStatus http_client_close(HttpClient *client);

#endif /* PG_HTTP_CLIENT_H */
