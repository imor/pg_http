#include "client.h"

typedef struct HttpClient {
  // Poller used for I/O and timer events
  Poller poller;

  CURLM *multi;
} HttpClient;

static HttpClientStatus register_callbacks(HttpClient *client);

HttpClientStatus http_client_init(HttpClient *client) {
  if (poller_create(&client->poller) < 0) {
    return HTTP_CLIENT_POLLER_ERROR;
  }

  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
    poller_close(&client->poller);
    return HTTP_CLIENT_CURL_ERROR;
  }

  client->multi = curl_multi_init();
  if (client->multi == NULL) {
    curl_global_cleanup();
    (void)poller_close(&client->poller);
    return HTTP_CLIENT_CURL_ERROR;
  }

  return register_callbacks(client->multi);
}

HttpClientStatus http_send(__attribute__((unused)) HttpClient *client,
                           __attribute__((unused)) Request    *requst) {
  return HTTP_CLIENT_OK;
}

HttpClientStatus http_client_close(HttpClient *client) {
  if (curl_multi_cleanup(client->multi) != CURLM_OK) {
    return HTTP_CLIENT_CURL_ERROR;
  }

  curl_global_cleanup();

  if (poller_close(&client->poller) < 0) {
    return HTTP_CLIENT_POLLER_ERROR;
  }

  return HTTP_CLIENT_OK;
}

/*
 * libcurl tells us which events it's interested in for a socket and
 * we register those events with the `client->poller`.
 *
 * This function return 0 on success and -1 on error.
 */
static int socket_callback(__attribute__((unused)) CURL         *easy,
                           __attribute__((unused)) curl_socket_t socket,
                           __attribute__((unused)) int what, __attribute__((unused)) void *clientp,
                           __attribute__((unused)) void *socketp) {
  // HttpClient *client = (HttpClient *)clientp;

  // PollerOp       op;
  // PollerInterest interest;

  // if (poller_socket_ctl(&client->poller, op, interest) < 0) {
  //   return -1;
  // }

  return 0;
}

/*
 * libcurl expects us to set a oneshot timer in the timer callback.
 * A timeout_ms value of zero means fire immediately. A value of
 * -1 means delete the timer. Any other positive value means we
 * set the timer for that many milliseconds.
 *
 * This function return 0 on success and -1 on error.
 */
static int timer_callback(__attribute__((unused)) CURLM *multi, long timeout_ms, void *clientp) {
  HttpClient *client = (HttpClient *)clientp;

  // Poller API only supports timeout in milliseconds
  if (timeout_ms > INT32_MAX || timeout_ms < -1) {
    return -1;
  }

  PollerOp op = timeout_ms == -1 ? POLLER_OP_DEL : POLLER_OP_ADD;

  if (poller_timer_ctl(&client->poller, op, (int)timeout_ms) < 0) {
    return -1;
  }

  return 0;
}

/*
 * Register socket and timer callbacks with libcurl.
 */
static HttpClientStatus register_callbacks(HttpClient *client) {
  // Tell libcurl to call `socket_callback` function when it needs
  // to register interest in socket events.
  if (curl_multi_setopt(client->multi, CURLMOPT_SOCKETFUNCTION, socket_callback) != CURLM_OK) {
    return HTTP_CLIENT_CURL_ERROR;
  }
  // Make libcurl pass `client` in the `clientp` argument in the `socket_callback`
  // function
  if (curl_multi_setopt(client->multi, CURLMOPT_SOCKETDATA, client) != CURLM_OK) {
    return HTTP_CLIENT_CURL_ERROR;
  }

  // Tell libcurl to call `socket_callback` function when it needs
  // to register a oneshot timer.
  if (curl_multi_setopt(client->multi, CURLMOPT_TIMERFUNCTION, timer_callback) != CURLM_OK) {
    return HTTP_CLIENT_CURL_ERROR;
  }
  // Make libcurl pass `client` in the `clientp` argument in the `timer_callback`
  // function
  if (curl_multi_setopt(client->multi, CURLMOPT_TIMERDATA, client) != CURLM_OK) {
    return HTTP_CLIENT_CURL_ERROR;
  }

  return HTTP_CLIENT_OK;
}
