#include "request.h"

#include <string.h>

static const char *method_name(HttpMethod m) {
  switch (m) {
  case HTTP_GET: return "GET";
  case HTTP_POST: return "POST";
  case HTTP_PUT: return "PUT";
  case HTTP_PATCH: return "PATCH";
  case HTTP_DELETE: return "DELETE";
  case HTTP_HEAD: return "HEAD";
  }
  return "GET";
}

int http_request_apply(const HttpRequest *req, CURL *easy, struct curl_slist **headers) {
  for (size_t i = 0; i < req->header_count; i++) {
    const HttpHeader *h = &req->headers[i];
    // curl copies the line into its slist node, so only this one temporary copy is needed
    char *line = psprintf("%.*s: %.*s", (int)h->name.len, h->name.data, (int)h->value.len,
                          h->value.data);
    struct curl_slist *l = curl_slist_append(*headers, line);
    pfree(line);
    if (l == NULL) return -1;
    *headers = l;
  }

  curl_easy_setopt(easy, CURLOPT_URL, req->url);
  curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, req->timeout_ms > 0 ? req->timeout_ms : 10000L);
  if (*headers) curl_easy_setopt(easy, CURLOPT_HTTPHEADER, *headers);

  if (req->method == HTTP_HEAD) {
    curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
  } else if (req->method != HTTP_GET) {
    curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method_name(req->method));
  }

  // POSTFIELDS is not copied by curl: it reads straight from the caller's buffer
  if (req->body.data != NULL && req->method != HTTP_HEAD) {
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, req->body.data);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)req->body.len);
  }

  return 0;
}
