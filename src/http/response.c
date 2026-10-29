#include "response.h"

#include <string.h>

void http_response_init(HttpResponse *response) {
  response->status_code = 0;
  response->error_type  = NULL;
  response->error_msg   = NULL;
  response->headers     = NULL;
  initStringInfo(&response->body);
}

size_t http_response_write_body(char *data, size_t size, size_t nmemb, void *userp) {
  HttpResponse *response = (HttpResponse *)userp;
  size_t        total    = size * nmemb;

  appendBinaryStringInfo(&response->body, data, (int)total);

  return total;
}

void http_response_collect_headers(HttpResponse *response, CURL *easy) {
  JsonbParseState *state = NULL;
  pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);

  struct curl_header *header = NULL;
  struct curl_header *prev   = NULL;
  // -1 selects the headers of the last request, i.e. after any redirects
  while ((header = curl_easy_nextheader(easy, CURLH_HEADER, -1, prev)) != NULL) {
    JsonbValue key   = {.type = jbvString,
                        .val  = {.string = {.len = (int)strlen(header->name), .val = header->name}}};
    JsonbValue value = {
      .type = jbvString,
      .val  = {.string = {.len = (int)strlen(header->value), .val = header->value}}};
    pushJsonbValue(&state, WJB_KEY, &key);
    pushJsonbValue(&state, WJB_VALUE, &value);
    prev = header;
  }

  JsonbValue *object = pushJsonbValue(&state, WJB_END_OBJECT, NULL);
  response->headers  = JsonbValueToJsonb(object);
}
