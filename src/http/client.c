#include "client.h"

typedef struct HttpClient {
  // Poller used for I/O and timer events
  Poller poller;

  CURLM *multi_handle;
} HttpClient;

HttpClientStatus http_client_init(HttpClient *client) {
  return poller_create(&client->poller);
}

HttpClientStatus http_send(__attribute__((unused)) HttpClient *client,
                           __attribute__((unused)) Request    *requst) {
  return HTTP_CLIENT_OK;
}

HttpClientStatus http_client_close(HttpClient *client) {
  return poller_close(&client->poller);
}
