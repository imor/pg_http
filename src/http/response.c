#include "response.h"

#include <float.h>
#include <string.h>

static HttpResponseStatus set_curl_easy_options(HttpResponse *response);

HttpResponseStatus http_response_init(HttpResponse *response) {
  response->easy                 = curl_easy_init();
  response->status_code          = 0;
  response->error_type           = NULL;
  response->error_msg            = NULL;
  response->headers              = NULL;
  response->request_headers      = NULL;
  response->requests_relid       = InvalidOid;
  response->timeout_milliseconds = 0;
  if (response->easy == NULL) {
    return HTTP_RESPONSE_CURL_ERROR;
  }

  initStringInfo(&response->body);

  return set_curl_easy_options(response);
}

void http_response_release_handle(HttpResponse *response) {
  if (response->easy == NULL) return;
  curl_easy_cleanup(response->easy);
  response->easy = NULL;

  // Only free once libcurl is done with the headers
  curl_slist_free_all(response->request_headers);
  response->request_headers = NULL;
}

void http_response_collect_headers(HttpResponse *response) {
  JsonbParseState *state = NULL;
  pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);

  struct curl_header *header = NULL;
  struct curl_header *prev   = NULL;
  // -1 selects the headers of the last request, i.e. after any redirects
  while ((header = curl_easy_nextheader(response->easy, CURLH_HEADER, -1, prev)) != NULL) {
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

/*
 * Gets a timing of the transfer in seconds, 0 if it isn't available
 */
static double timing(CURL *easy, CURLINFO info) {
  double value = 0;
  if (curl_easy_getinfo(easy, info, &value) != CURLE_OK) return 0;
  return value;
}

/*
 * Shows which step (DNS, TCP/SSL handshake or HTTP request/response) the transfer timed out in
 * and how long each took. If a step surpasses the timeout, its timing is 0 but its duration can
 * still be determined from the total time. If a step is omitted (e.g. SSL on plain HTTP) its
 * timing is 0 as well. The pretransfer time is greater than 0 once the HTTP step started.
 * See https://blog.cloudflare.com/a-question-of-timing/
 */
char *http_response_timeout_message(HttpResponse *response) {
  CURL  *easy        = response->easy;
  double namelookup  = timing(easy, CURLINFO_NAMELOOKUP_TIME);
  double connect     = timing(easy, CURLINFO_CONNECT_TIME);
  double appconnect  = timing(easy, CURLINFO_APPCONNECT_TIME);
  double pretransfer = timing(easy, CURLINFO_PRETRANSFER_TIME);
  double total       = timing(easy, CURLINFO_TOTAL_TIME);

  bool at_dns       = namelookup < DBL_EPSILON && connect < DBL_EPSILON;
  bool at_handshake = !at_dns && pretransfer < DBL_EPSILON;

  double dns       = at_dns ? total : namelookup;
  double handshake = 0;
  double http      = 0;

  if (at_handshake) {
    handshake = total - namelookup;
  } else if (!at_dns) {
    // appconnect is 0 when no SSL is involved, and the difference would be negative
    handshake = (connect - namelookup) + (appconnect > 0 ? appconnect - connect : 0);
    http      = total - pretransfer;
  }

  return psprintf("Timeout of %d ms reached. Total time: %f ms (DNS time: %f ms, TCP/SSL "
                  "handshake time: %f ms, HTTP Request/Response time: %f ms)",
                  response->timeout_milliseconds, total * 1000, dns * 1000, handshake * 1000,
                  http * 1000);
}

HttpResponseStatus http_response_close(HttpResponse *response) {
  http_response_release_handle(response);
  pfree(response->body.data);
  if (response->error_msg) pfree(response->error_msg);
  if (response->headers) pfree(response->headers);
  return HTTP_RESPONSE_OK;
}

static size_t write_callback(char *data, size_t size, size_t nmemb, void *userp) {
  HttpResponse *response = (HttpResponse *)userp;
  size_t        total    = size * nmemb;

  appendBinaryStringInfo(&response->body, data, (int)total);

  return total;
}

static HttpResponseStatus set_curl_easy_options(HttpResponse *response) {
  if (curl_easy_setopt(response->easy, CURLOPT_WRITEFUNCTION, write_callback) != CURLE_OK) {
    return HTTP_RESPONSE_CURL_ERROR;
  }
  if (curl_easy_setopt(response->easy, CURLOPT_WRITEDATA, response) != CURLE_OK) {
    return HTTP_RESPONSE_CURL_ERROR;
  }

  return HTTP_RESPONSE_OK;
}
