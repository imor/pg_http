#include "client.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <lib/stringinfo.h>
#include <utils/memutils.h>

#define CLIENT_MAX_EVENTS 64

/*
 * One request/response exchange. It owns the request, the response being
 * filled in and the curl handle, and is attached to the easy handle via
 * CURLOPT_PRIVATE.
 */
typedef struct Transfer {
  HttpRequest        req; // borrows its memory from the caller
  HttpResponse       res;
  CURL              *easy;
  struct curl_slist *req_headers;
  char               errbuf[CURL_ERROR_SIZE];
} Transfer;

static size_t write_cb(char *data, size_t size, size_t nmemb, void *userp) {
  http_response_write_body(&((Transfer *)userp)->res, data, size * nmemb);
  return size * nmemb;
}

static size_t header_cb(char *data, size_t size, size_t nmemb, void *userp) {
  http_response_write_header(&((Transfer *)userp)->res, data, size * nmemb);
  return size * nmemb;
}

/*
 * libcurl asks us to call curl_multi_socket_action after `timeout_ms`
 * (zero means now, negative means cancel the timer).
 */
static int timer_cb(__attribute__((unused)) CURLM *multi, long timeout_ms, void *userp) {
  HttpClient *client = (HttpClient *)userp;

  if (timeout_ms > INT32_MAX) timeout_ms = INT32_MAX;

  // libcurl passes -1 to indicate the timer should be deleted
  PollerOp op = timeout_ms >= 0 ? POLLER_OP_ADD : POLLER_OP_DEL;

  if (poller_timer_ctl(&client->poller, op, (int)timeout_ms) < 0) {
    int err = errno;
    ereport(WARNING, errmsg("pg_http: could not set timer: %s", strerror(err)));
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
  if (poller_socket_ctl(&client->poller, op, in) < 0 &&
      !(remove && (errno == ENOENT || errno == EBADF))) {
    int err = errno;
    ereport(WARNING,
            errmsg("pg_http: could not update fd %d in poller: %s", sockfd, strerror(err)));
    return -1;
  }

  curl_multi_assign(client->multi, sockfd, remove ? NULL : (void *)((uintptr_t)in.new_events + 1));
  return 0;
}

int http_client_init(HttpClient *client) {
  if (poller_create(&client->poller) < 0) return -1;

  curl_global_init(CURL_GLOBAL_DEFAULT);
  client->multi = curl_multi_init();
  if (client->multi == NULL) {
    curl_global_cleanup();
    poller_close(&client->poller);
    return -1;
  }

  client->running  = 0;
  client->finished = NULL;

  curl_multi_setopt(client->multi, CURLMOPT_SOCKETFUNCTION, socket_cb);
  curl_multi_setopt(client->multi, CURLMOPT_SOCKETDATA, client);
  curl_multi_setopt(client->multi, CURLMOPT_TIMERFUNCTION, timer_cb);
  curl_multi_setopt(client->multi, CURLMOPT_TIMERDATA, client);

  return 0;
}

int http_client_fd(HttpClient *client) {
  return client->poller.fd;
}

static void transfer_free(HttpClient *client, Transfer *t) {
  if (t->easy) {
    curl_multi_remove_handle(client->multi, t->easy);
    curl_easy_cleanup(t->easy);
  }
  curl_slist_free_all(t->req_headers);
  http_response_free(&t->res);
  pfree(t);
}

const HttpResponse *http_client_next(HttpClient *client) {
  // The previously returned response is released now
  if (client->finished != NULL) {
    transfer_free(client, client->finished);
    client->finished = NULL;
  }

  CURLMsg *msg;
  int      msgs_left;

  while ((msg = curl_multi_info_read(client->multi, &msgs_left)) != NULL) {
    if (msg->msg != CURLMSG_DONE) continue;

    Transfer *t    = NULL;
    long      code = 0;

    curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &t);
    curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &code);

    CURLcode    result = msg->data.result;
    const char *err    = t->errbuf[0] ? t->errbuf : curl_easy_strerror(result);

    http_response_finish(&t->res, (int)result, err, (int)code);

    // The easy handle is no longer needed, but the response stays until the next call
    curl_multi_remove_handle(client->multi, t->easy);
    curl_easy_cleanup(t->easy);
    t->easy          = NULL;
    client->finished = t;
    return &t->res;
  }

  return NULL;
}

int http_client_send(HttpClient *client, const HttpRequest *req) {
  // Transfers outlive any short-lived memory context the caller may be in
  MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
  Transfer     *t   = palloc0(sizeof(Transfer));
  MemoryContextSwitchTo(old);

  t->req = *req;
  http_response_init(&t->res, &t->req);

  t->easy = curl_easy_init();
  if (t->easy == NULL || http_request_apply(&t->req, t->easy, &t->req_headers) < 0) {
    transfer_free(client, t);
    return -1;
  }

  CURL *e = t->easy;
  curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(e, CURLOPT_WRITEDATA, t);
  curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, header_cb);
  curl_easy_setopt(e, CURLOPT_HEADERDATA, t);
  curl_easy_setopt(e, CURLOPT_PRIVATE, t);
  curl_easy_setopt(e, CURLOPT_ERRORBUFFER, t->errbuf);
  curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);

  if (curl_multi_add_handle(client->multi, e) != CURLM_OK) {
    transfer_free(client, t);
    return -1;
  }

  // Kick off the transfer. libcurl will ask for a timer and sockets via the callbacks.
  curl_multi_socket_action(client->multi, CURL_SOCKET_TIMEOUT, 0, &client->running);
  return 0;
}

int http_client_process(HttpClient *client) {
  PollerEvent evs[CLIENT_MAX_EVENTS];
  int         n;

  do {
    n = poller_wait(&client->poller, evs, CLIENT_MAX_EVENTS, 0);

    if (n < 0) {
      if (errno == EINTR) return 0;
      int err = errno;
      ereport(WARNING, errmsg("pg_http: poller_wait failed: %s", strerror(err)));
      return -1;
    }

    for (int i = 0; i < n; i++) {
      uint32_t flags = poller_event_kind(&client->poller, &evs[i]);

      if (flags & POLLER_EV_TIMER) {
        curl_multi_socket_action(client->multi, CURL_SOCKET_TIMEOUT, 0, &client->running);
        continue;
      }

      int mask = 0;
      if (flags & POLLER_EV_READ) mask |= CURL_CSELECT_IN;
      if (flags & POLLER_EV_WRITE) mask |= CURL_CSELECT_OUT;
      if (flags & POLLER_EV_ERROR) mask |= CURL_CSELECT_ERR;

      curl_multi_socket_action(client->multi, poller_event_fd(&client->poller, &evs[i]), mask,
                               &client->running);
    }
  } while (n == CLIENT_MAX_EVENTS);

  return 0;
}

void http_client_close(HttpClient *client) {
  CURL **easies = curl_multi_get_handles(client->multi);

  if (easies != NULL) {
    for (int i = 0; easies[i] != NULL; i++) {
      Transfer *t = NULL;
      curl_easy_getinfo(easies[i], CURLINFO_PRIVATE, &t);
      if (t != NULL) transfer_free(client, t);
    }
    curl_free(easies);
  }

  if (client->finished != NULL) transfer_free(client, client->finished);
  client->finished = NULL;

  curl_multi_cleanup(client->multi);
  poller_close(&client->poller);
  client->multi   = NULL;
  client->running = 0;
  curl_global_cleanup();
}
