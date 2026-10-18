#include "response.h"

#include <string.h>

#include <utils/builtins.h>
#include <utils/memutils.h>

static bool is_ws(char c) {
  return c == ' ' || c == '\t';
}

/*
 * Parses the raw header lines into `out` (sized by http_response_max_headers). The
 * resulting slices point into `raw`, nothing is copied. Returns the count.
 */
size_t http_response_parse_headers(const char *raw, size_t len, HttpHeader *out) {
  const char *p   = raw;
  const char *end = raw + len;
  size_t      n   = 0;

  while (p < end) {
    const char *eol = memchr(p, '\n', (size_t)(end - p));
    const char *next = eol ? eol + 1 : end;
    if (eol == NULL) eol = end;
    if (eol > p && eol[-1] == '\r') eol--;

    const char *colon = memchr(p, ':', (size_t)(eol - p));
    if (colon != NULL && colon > p) {
      const char *v = colon + 1;
      while (v < eol && is_ws(*v)) v++;
      const char *ve = eol;
      while (ve > v && is_ws(ve[-1])) ve--;

      out[n].name  = (HttpSlice){p, (size_t)(colon - p)};
      out[n].value = (HttpSlice){v, (size_t)(ve - v)};
      n++;
    }
    p = next;
  }

  return n;
}

size_t http_response_max_headers(const char *raw, size_t len) {
  size_t n = 1;
  for (size_t i = 0; i < len; i++)
    if (raw[i] == '\n') n++;
  return n;
}

const HttpHeader *http_response_header(const HttpResponse *res, const char *name) {
  size_t len = strlen(name);

  for (size_t i = 0; i < res->header_count; i++) {
    const HttpHeader *h = &res->headers[i];
    if (h->name.len == len && pg_strncasecmp(h->name.data, name, len) == 0) return h;
  }

  return NULL;
}

void http_response_init(HttpResponse *res, const HttpRequest *req) {
  *res = (HttpResponse){.request = req};

  // Responses outlive any short-lived memory context the caller may be in
  MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
  initStringInfo(&res->raw_headers);
  initStringInfo(&res->body_buf);
  MemoryContextSwitchTo(old);
}

void http_response_write_body(HttpResponse *res, const char *data, size_t len) {
  appendBinaryStringInfo(&res->body_buf, data, (int)len);
}

void http_response_write_header(HttpResponse *res, const char *data, size_t len) {
  // A new status line starts a new header block (redirects, 100-continue), drop earlier ones
  if (len >= 5 && memcmp(data, "HTTP/", 5) == 0) resetStringInfo(&res->raw_headers);

  appendBinaryStringInfo(&res->raw_headers, data, (int)len);
}

void http_response_finish(HttpResponse *res, int curl_code, const char *error, int status) {
  size_t raw_len = (size_t)res->raw_headers.len;

  MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
  res->header_array = palloc(sizeof(HttpHeader) * http_response_max_headers(res->raw_headers.data, raw_len));
  MemoryContextSwitchTo(old);

  res->curl_code    = curl_code;
  res->error        = curl_code != 0 ? error : NULL;
  res->status       = status;
  res->headers      = res->header_array;
  res->header_count = http_response_parse_headers(res->raw_headers.data, raw_len, res->header_array);
  res->body         = (HttpSlice){res->body_buf.data, (size_t)res->body_buf.len};
}

void http_response_free(HttpResponse *res) {
  if (res->header_array) pfree(res->header_array);
  pfree(res->raw_headers.data);
  pfree(res->body_buf.data);
  *res = (HttpResponse){0};
}
