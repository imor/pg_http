#include "response.h"

static HttpResponseStatus set_curl_easy_options(HttpResponse *response);

HttpResponseStatus http_response_init(HttpResponse *response) {
  response->easy = curl_easy_init();
  if (response->easy == NULL) {
    return HTTP_RESPONSE_CURL_ERROR;
  }

  initStringInfo(&response->body);

  return set_curl_easy_options(response);
}

HttpResponseStatus http_response_close(HttpResponse *response) {
  curl_easy_cleanup(response->easy);
  pfree(response->body.data);
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