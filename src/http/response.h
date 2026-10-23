#ifndef PG_HTTP_RESPONSE_H
#define PG_HTTP_RESPONSE_H

#include "pg_prelude.h"

#include <curl/curl.h>

/*
 * And HTTP response. Contains the easy CURL handle
 * and the response body.
 */
typedef struct HttpResponse {
  // The id of the request in the queue table which this response is for
  int64 request_id;

  // CURL's easy handle for this HTTP transfer
  CURL *easy;

  // The response body
  StringInfoData body;
} HttpResponse;

typedef enum {
  HTTP_RESPONSE_OK         = 0,
  HTTP_RESPONSE_CURL_ERROR = -1,
} HttpResponseStatus;

/*
 * Initialize an HttpResponse
 */
HttpResponseStatus http_response_init(HttpResponse *response);

/*
 * Destroy an HttpResponse
 */
HttpResponseStatus http_response_close(HttpResponse *response);

#endif /* PG_HTTP_RESPONSE_H */
