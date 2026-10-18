#ifndef PG_HTTP_RESPONSE_H
#define PG_HTTP_RESPONSE_H

#include "request.h"

#include <lib/stringinfo.h>

/*
 * The response to a request. It lives inside the client's transfer for that
 * request and is filled in place by libcurl's callbacks, so the body and
 * headers are written once and never copied. Headers are slices pointing into
 * the raw header buffer.
 *
 * The public fields are valid once the transfer has finished. Do not touch
 * the private ones.
 */
typedef struct HttpResponse {
  const HttpRequest *request; // the request this answers
  int                curl_code; // 0 on transport success, else a CURLcode
  const char        *error; // curl error message, NULL on success

  int status; // HTTP status code, 0 if no response was received

  const HttpHeader *headers;
  size_t            header_count;

  HttpSlice body;

  // private
  StringInfoData raw_headers;
  StringInfoData body_buf;
  HttpHeader    *header_array;
} HttpResponse;

/* Prepares an empty response for `req`. */
void http_response_init(HttpResponse *res, const HttpRequest *req);

/* Append received bytes. Same shape as what libcurl delivers. */
void http_response_write_body(HttpResponse *res, const char *data, size_t len);
void http_response_write_header(HttpResponse *res, const char *data, size_t len);

/*
 * Seals the response once the transfer is done: parses the headers and
 * publishes the body slice. `error` is only used when curl_code != 0 and must
 * outlive the response.
 */
void http_response_finish(HttpResponse *res, int curl_code, const char *error, int status);

/* Releases everything the response owns. */
void http_response_free(HttpResponse *res);

/* Upper bound on the number of headers in `raw`, for sizing the output array. */
size_t http_response_max_headers(const char *raw, size_t len);

/*
 * Parses raw "Name: value\r\n" lines into `out`. The slices point into `raw`,
 * nothing is copied. Returns the number of headers written.
 */
size_t http_response_parse_headers(const char *raw, size_t len, HttpHeader *out);

/* Returns the first header matching `name` (case-insensitive), or NULL. */
const HttpHeader *http_response_header(const HttpResponse *res, const char *name);

#endif /* PG_HTTP_RESPONSE_H */
