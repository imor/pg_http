#ifndef PG_HTTP_RESPONSE_H
#define PG_HTTP_RESPONSE_H

#include "pg_prelude.h"

#include <curl/curl.h>
#include <utils/jsonb.h>

/*
 * An HTTP response. Contains the response body, headers and
 * status, or the error if the request failed.
 */
typedef struct HttpResponse {
  // The response headers, NULL if the request failed
  Jsonb *headers;

  // The response body
  StringInfoData body;

  // The HTTP status code, 0 if the request failed
  int32 status_code;

  // NULL if the request did not fail, otherwise one of the values of http.error_type
  const char *error_type;

  // NULL if the request did not fail, otherwise a description of the error
  char *error_msg;
} HttpResponse;

/*
 * Initialize an HttpResponse, allocating in the current memory context
 */
void http_response_init(HttpResponse *response);

/*
 * Fills in `headers` from the final response of the finished transfer with the easy handle
 * `easy`. Must be called before the handle is released. Allocates in the current memory context.
 */
void http_response_collect_headers(HttpResponse *response, CURL *easy);

#endif /* PG_HTTP_RESPONSE_H */
