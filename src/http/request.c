#include "request.h"
#include "pg_prelude.h"

#include <string.h>

/*
 * A header with a CR or LF in it can inject extra headers or a body into the request, since
 * libcurl ends every header with CRLF. The message only names the header, the value may hold
 * credentials.
 */
static char *crlf_header_rejection(const char *header) {
  size_t bad = strcspn(header, "\r\n");
  if (header[bad] == '\0') return NULL;

  size_t name_len = strcspn(header, ":");
  if (name_len > bad) name_len = bad;

  return psprintf("header \"%.*s\" contains a carriage return or line feed", (int)name_len, header);
}

char *http_request_rejection(const HttpRequest *request, int max_timeout_ms) {
  // libcurl treats a 0 timeout as no timeout, and a request that never finishes would be kept
  // forever, so requests outside the bound are not sent.
  if (request->timeout_milliseconds <= 0 || request->timeout_milliseconds > max_timeout_ms) {
    return psprintf("timeout_milliseconds must be between 1 and %d (" EXTNAME
                    ".max_timeout_ms), got %d",
                    max_timeout_ms, request->timeout_milliseconds);
  }

  for (int i = 0; i < request->num_headers; i++) {
    char *reason = crlf_header_rejection(request->headers[i]);
    if (reason != NULL) return reason;
  }

  if (pg_strcasecmp(request->method, "GET") != 0 && pg_strcasecmp(request->method, "POST") != 0 &&
      pg_strcasecmp(request->method, "DELETE") != 0) {
    return psprintf("Unsupported request method %s", request->method);
  }

  return NULL;
}
