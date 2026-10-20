#include "client.h"

int http_client_init(HttpClient *client) {
  return poller_create(&client->poller);
}

int http_send(__attribute__((unused)) HttpClient *client, __attribute__((unused)) Request *requst) {
  return 0;
}

int http_client_close(HttpClient *client) {
  return poller_close(&client->poller);
}
