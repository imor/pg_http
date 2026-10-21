#ifndef PG_HTTP_RESPONSE_H
#define PG_HTTP_RESPONSE_H

#include "pg_prelude.h"

#include <curl/curl.h>

typedef struct HttpResponse {
  CURL          *easy;
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
