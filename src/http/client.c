#include "client.h"

#include <errno.h>
#include <stdint.h>

#include <lib/stringinfo.h>
#include <utils/memutils.h>

#define CLIENT_MAX_EVENTS 64

/*
 * State of one request, attached to its easy handle via CURLOPT_PRIVATE.
 */
typedef struct HttpRequest {
  CURL          *easy;
  StringInfoData body;
} HttpRequest;

static size_t write_cb(char *data, size_t size, size_t nmemb, void *userp) {
  HttpRequest *req   = (HttpRequest *)userp;
  size_t       total = size * nmemb;

  appendBinaryStringInfo(&req->body, data, (int)total);

  return total;
}

/*
 * libcurl asks us to call curl_multi_socket_action after `timeout_ms`
 * (zero means now, negative means cancel the timer).
 */
static int timer_cb(__attribute__((unused)) CURLM *multi, long timeout_ms, void *userp) {
  HttpClient *client = (HttpClient *)userp;

  if (timeout_ms > INT32_MAX) timeout_ms = INT32_MAX;

  // libcurl passes -1 to indicate the timer should be deleted
  PollerInterest in = {.fd = -1, .timeout_ms = (int)timeout_ms};
  if (timeout_ms < 0)
    in.old_events = POLLER_EV_TIMER;
  else
    in.new_events = POLLER_EV_TIMER;

  if (poller_ctl(client->poller, timeout_ms < 0 ? POLLER_OP_DEL : POLLER_OP_ADD, &in) < 0) {
    ereport(WARNING, errmsg("pg_http: could not set timer: %m"));
    return -1;
  }

  return 0;
}

/*
 * libcurl tells us which events it wants for a socket. The socketp pointer
 * is used to remember the events currently registered with the poller
 * (offset by one so that null means "not registered"). The kqueue poller
 * needs the old events to modify or delete an fd.
 */
static int socket_cb(__attribute__((unused)) CURL *easy, curl_socket_t sockfd, int what,
                     void *userp, void *socketp) {
  HttpClient    *client = (HttpClient *)userp;
  PollerInterest in     = {.fd = sockfd};

  if (socketp != NULL) in.old_events = (uint32_t)((uintptr_t)socketp - 1);

  bool remove = what == CURL_POLL_REMOVE;

  // libcurl may remove a socket it never registered
  if (remove && socketp == NULL) return 0;

  // CURL_POLL_REMOVE has neither the IN nor the OUT bit, so new_events stays empty for it
  if (what & CURL_POLL_IN) in.new_events |= POLLER_EV_READ;
  if (what & CURL_POLL_OUT) in.new_events |= POLLER_EV_WRITE;

  PollerOp op = remove ? POLLER_OP_DEL : socketp == NULL ? POLLER_OP_ADD : POLLER_OP_MOD;

  // when removing, the fd may already be closed, which also removes it from the poller
  if (poller_ctl(client->poller, op, &in) < 0 && !(remove && (errno == ENOENT || errno == EBADF))) {
    ereport(WARNING, errmsg("pg_http: could not update fd %d in poller: %m", sockfd));
    return -1;
  }

  curl_multi_assign(client->multi, sockfd, remove ? NULL : (void *)((uintptr_t)in.new_events + 1));
  return 0;
}

int http_client_init(HttpClient *client, Poller *poller) {
  client->poller  = poller;
  client->running = 0;
  client->multi   = curl_multi_init();

  if (client->multi == NULL) return -1;

  curl_multi_setopt(client->multi, CURLMOPT_SOCKETFUNCTION, socket_cb);
  curl_multi_setopt(client->multi, CURLMOPT_SOCKETDATA, client);
  curl_multi_setopt(client->multi, CURLMOPT_TIMERFUNCTION, timer_cb);
  curl_multi_setopt(client->multi, CURLMOPT_TIMERDATA, client);

  return 0;
}

static void request_free(HttpClient *client, HttpRequest *req) {
  curl_multi_remove_handle(client->multi, req->easy);
  curl_easy_cleanup(req->easy);
  pfree(req->body.data);
  pfree(req);
}

void http_client_close(HttpClient *client) {
  if (client->multi == NULL) return;

  CURL **easies = curl_multi_get_handles(client->multi);

  if (easies != NULL) {
    for (int i = 0; easies[i] != NULL; i++) {
      HttpRequest *req = NULL;
      curl_easy_getinfo(easies[i], CURLINFO_PRIVATE, &req);
      if (req != NULL) request_free(client, req);
    }
    curl_free(easies);
  }

  curl_multi_cleanup(client->multi);
  client->multi   = NULL;
  client->running = 0;
}

/*
 * Reports and frees all finished transfers.
 */
static void check_finished(HttpClient *client) {
  CURLMsg *msg;
  int      msgs_left;

  while ((msg = curl_multi_info_read(client->multi, &msgs_left)) != NULL) {
    if (msg->msg != CURLMSG_DONE) continue;

    HttpRequest *req  = NULL;
    long         code = 0;

    curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &req);
    curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &code);

    if (msg->data.result == CURLE_OK) {
      ereport(LOG, errmsg("pg_http: request finished with status %ld, %d body bytes", code,
                          req->body.len));
    } else {
      ereport(LOG, errmsg("pg_http: request failed: %s", curl_easy_strerror(msg->data.result)));
    }

    request_free(client, req);
  }
}

int http_client_get(HttpClient *client, const char *url) {
  // Requests outlive any short-lived memory context the caller may be in
  MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
  HttpRequest  *req = palloc0(sizeof(HttpRequest));
  initStringInfo(&req->body);
  MemoryContextSwitchTo(old);

  req->easy = curl_easy_init();
  if (req->easy == NULL) {
    pfree(req->body.data);
    pfree(req);
    return -1;
  }

  curl_easy_setopt(req->easy, CURLOPT_URL, url);
  curl_easy_setopt(req->easy, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(req->easy, CURLOPT_WRITEDATA, req);
  curl_easy_setopt(req->easy, CURLOPT_PRIVATE, req);
  curl_easy_setopt(req->easy, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(req->easy, CURLOPT_TIMEOUT_MS, 10000L);

  if (curl_multi_add_handle(client->multi, req->easy) != CURLM_OK) {
    curl_easy_cleanup(req->easy);
    pfree(req->body.data);
    pfree(req);
    return -1;
  }

  // Kick off the transfer. libcurl will ask for a timer and sockets via the callbacks.
  curl_multi_socket_action(client->multi, CURL_SOCKET_TIMEOUT, 0, &client->running);
  check_finished(client);

  return 0;
}

int http_client_process(HttpClient *client) {
  PollerEvent evs[CLIENT_MAX_EVENTS];
  int         n;

  do {
    n = poller_wait(client->poller, evs, CLIENT_MAX_EVENTS, 0);

    if (n < 0) {
      if (errno == EINTR) return 0;
      ereport(WARNING, errmsg("pg_http: poller_wait failed: %m"));
      return -1;
    }

    for (int i = 0; i < n; i++) {
      uint32_t flags = poller_event_kind(client->poller, &evs[i]);

      if (flags & POLLER_EV_TIMER) {
        curl_multi_socket_action(client->multi, CURL_SOCKET_TIMEOUT, 0, &client->running);
        continue;
      }

      int mask = 0;
      if (flags & POLLER_EV_READ) mask |= CURL_CSELECT_IN;
      if (flags & POLLER_EV_WRITE) mask |= CURL_CSELECT_OUT;

      curl_multi_socket_action(client->multi, poller_event_fd(client->poller, &evs[i]), mask,
                               &client->running);
    }
  } while (n == CLIENT_MAX_EVENTS);

  check_finished(client);

  return 0;
}
