#include "client.h"
#include "pg_prelude.h"
#include "response.h"

#include <curl/curl.h>
#include <stdbool.h>

typedef struct HttpClient {
  // Poller used for I/O and timer events
  Poller poller;

  // Curl's multi handle
  CURLM *multi;

  // Number of curl transfers still in progress
  int running_handles;
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

HttpClientStatus http_client_send(HttpClient *client, HttpRequest *request,
                                  HttpResponse *response) {
  if (curl_easy_setopt(response->easy, CURLOPT_URL, request->url) != CURLE_OK) {
    return HTTP_CLIENT_CURL_ERROR;
  }
  if (curl_easy_setopt(response->easy, CURLOPT_TIMEOUT_MS, 10000) != CURLE_OK) {
    return HTTP_CLIENT_CURL_ERROR;
  }
  if (curl_easy_setopt(response->easy, CURLOPT_PRIVATE, response) != CURLE_OK) {
    return HTTP_CLIENT_CURL_ERROR;
  }

  if (curl_multi_add_handle(client->multi, response->easy) != CURLM_OK) {
    return HTTP_CLIENT_CURL_ERROR;
  }

  if (curl_multi_socket_action(client->multi, CURL_SOCKET_TIMEOUT, 0, &client->running_handles) !=
      CURLM_OK) {
    return HTTP_CLIENT_CURL_ERROR;
  }

  return HTTP_CLIENT_OK;
}

/*
 * Maximum number of events read from the poller in one `poller_wait` call
 */
#define MAX_EVENTS 64

HttpClientStatus http_client_process(HttpClient *client) {
  PollerEvent events[MAX_EVENTS];
  int         num_events;

  do {
    // Poll events without blocking
    num_events = poller_wait(&client->poller, events, MAX_EVENTS, 0);

    if (num_events < 0) {
      // On an interrupt we return ok to let the caller retry
      if (errno == EINTR) return HTTP_CLIENT_OK;

      // Otherwise it's an error
      return HTTP_CLIENT_POLLER_ERROR;
    }

    // Now process the events we got back by calling appropriate curl functions
    for (int i = 0; i < num_events; i++) {
      uint32_t flags = poller_event_kind(&client->poller, &events[i]);

      // Handle a timer event
      if (flags & POLLER_EV_TIMER) {
        if (curl_multi_socket_action(client->multi, CURL_SOCKET_TIMEOUT, 0,
                                     &client->running_handles) != CURLM_OK) {
          return HTTP_CLIENT_CURL_ERROR;
        }
        continue;
      }

      // Handle socket I/O events
      int mask = 0;
      if (flags & POLLER_EV_READ) mask |= CURL_CSELECT_IN;
      if (flags & POLLER_EV_WRITE) mask |= CURL_CSELECT_OUT;
      if (flags & POLLER_EV_ERROR) mask |= CURL_CSELECT_ERR;

      if (curl_multi_socket_action(client->multi, poller_event_fd(&client->poller, &events[i]),
                                   mask, &client->running_handles) != CURLM_OK) {
        return HTTP_CLIENT_CURL_ERROR;
      }
    }
  } while (num_events == MAX_EVENTS);

  return HTTP_CLIENT_OK;
}

HttpClientStatus http_client_get_responses(HttpClient *client, response_callback callback) {
  CURLMsg *message;
  int      remaining_messages;

  while ((message = curl_multi_info_read(client->multi, &remaining_messages)) != NULL) {
    if (message->msg != CURLMSG_DONE) continue;

    HttpResponse *response    = NULL;
    long          status_code = 0;

    if (curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &response) != CURLE_OK) {
      return HTTP_CLIENT_CURL_ERROR;
    }

    if (curl_easy_getinfo(message->easy_handle, CURLINFO_RESPONSE_CODE, &status_code) != CURLE_OK) {
      return HTTP_CLIENT_CURL_ERROR;
    }

    callback(message->data.result, response, status_code);
  }

  return HTTP_CLIENT_OK;
}

int http_client_fd(HttpClient *client) {
  return client->poller.fd;
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
 * we register those events with `client->poller`.
 *
 * This function return 0 on success and -1 on error.
 */
static int socket_callback(__attribute__((unused)) CURL *easy, curl_socket_t sockfd, int what,
                           void *clientp, void *socketp) {
  HttpClient *client = (HttpClient *)clientp;

  PollerOp op;

  bool add    = socketp == NULL;
  bool remove = what & CURL_POLL_REMOVE;

  if (add) {
    op = POLLER_OP_ADD;
  } else if (remove) {
    op = POLLER_OP_DEL;
  } else {
    op = POLLER_OP_MOD;
  }

  PollerInterest interest = {.fd = sockfd};
  if (what & CURL_POLL_IN) {
    interest.new_events |= POLLER_EV_READ;
  }
  if (what & CURL_POLL_OUT) {
    interest.new_events |= POLLER_EV_WRITE;
  }

  if (op == POLLER_OP_ADD) {
    interest.old_events = POLLER_EV_NONE;
  } else {
    interest.old_events = (uint32_t)(uintptr_t)socketp;
  }

  if (curl_multi_assign(client->multi, sockfd,
                        (op == POLLER_OP_ADD || op == POLLER_OP_MOD)
                            ? (void *)(uintptr_t)interest.new_events
                            : NULL) != CURLM_OK) {
    return -1;
  }

  if (poller_socket_ctl(&client->poller, op, interest) < 0) {
    return -1;
  }

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
