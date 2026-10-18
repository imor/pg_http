#ifndef PG_HTTP_REQUEST_H
#define PG_HTTP_REQUEST_H

#include "pg_prelude.h"

// pragmas needed to pass compiling with -Wextra
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <curl/curl.h>

#pragma GCC diagnostic pop

/* A borrowed, non-owning view of bytes. Never copied by the client. */
typedef struct HttpSlice {
  const char *data;
  size_t      len;
} HttpSlice;

typedef enum HttpMethod {
  HTTP_GET,
  HTTP_POST,
  HTTP_PUT,
  HTTP_PATCH,
  HTTP_DELETE,
  HTTP_HEAD,
} HttpMethod;

/* One header as "name: value" views. */
typedef struct HttpHeader {
  HttpSlice name;
  HttpSlice value;
} HttpHeader;

/*
 * A request to be made. It only borrows its memory: url, headers and body
 * must stay valid until the response callback fires (libcurl is pointed at
 * the body directly, and headers are only copied into curl_slist nodes, which
 * is unavoidable).
 */
typedef struct HttpRequest {
  HttpMethod        method;
  const char       *url; // NUL terminated
  const HttpHeader *headers;
  size_t            header_count;
  HttpSlice         body; // empty when data == NULL
  long              timeout_ms; // 0 = client default
  void             *user_data; // returned untouched in the response
} HttpRequest;

/*
 * Configures `easy` from `req` (url, method, headers, timeout, body). The
 * body is not copied. Headers are appended to `*headers`, which the caller
 * owns and must free with curl_slist_free_all even on failure. Returns zero
 * on success and -1 on error.
 */
int http_request_apply(const HttpRequest *req, CURL *easy, struct curl_slist **headers);

#endif /* PG_HTTP_REQUEST_H */
