#ifndef PG_HTTP_RESPONSE_H
#define PG_HTTP_RESPONSE_H

#include "pg_prelude.h"

#include <curl/curl.h>
#include <utils/jsonb.h>

/*
 * And HTTP response. Contains the easy CURL handle
 * and the response body.
 */
typedef struct HttpResponse {
  // The id of the request in the queue table which this response is for
  int64 request_id;

  // CURL's easy handle for this HTTP transfer. NULL once released.
  CURL *easy;

  // The headers sent with the request. libcurl doesn't copy them, so they are freed with the
  // handle.
  struct curl_slist *request_headers;

  // The oid of the requests table when the request was read from it. Used to drop the response
  // if the extension was dropped, and maybe created again, in the meantime.
  Oid requests_relid;

  // The timeout of the request, used for the error message when it times out
  int32 timeout_milliseconds;

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

typedef enum {
  HTTP_RESPONSE_OK         = 0,
  HTTP_RESPONSE_CURL_ERROR = -1,
} HttpResponseStatus;

/*
 * Initialize an HttpResponse
 */
HttpResponseStatus http_response_init(HttpResponse *response);

/*
 * Release the CURL easy handle, keeping the response data. Must only be
 * called once the transfer is finished and removed from the multi handle.
 */
void http_response_release_handle(HttpResponse *response);

/*
 * Fills in `headers` from the final response of the finished transfer. Must be
 * called before the handle is released. Allocates in the current memory context.
 */
void http_response_collect_headers(HttpResponse *response);

/*
 * Returns a palloc'd message with the details of a timed out transfer: the time spent in each
 * step. Must be called before the handle is released.
 */
char *http_response_timeout_message(HttpResponse *response);

/*
 * Destroy an HttpResponse
 */
HttpResponseStatus http_response_close(HttpResponse *response);

#endif /* PG_HTTP_RESPONSE_H */
