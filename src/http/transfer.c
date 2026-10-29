#include "transfer.h"

#include <float.h>

/*
 * Frees the transfer and returns its result
 */
static HttpResult *end(HttpTransfer *transfer) {
  HttpResult *result = transfer->result;

  if (transfer->easy != NULL) curl_easy_cleanup(transfer->easy);
  // Only free once libcurl is done with the headers
  curl_slist_free_all(transfer->request.headers);
  pfree(transfer);

  return result;
}

HttpTransfer *http_transfer_create(HttpRequest *request, Oid requests_relid) {
  HttpResult   *result   = http_result_create(request, requests_relid);
  HttpTransfer *transfer = MemoryContextAlloc(result->context, sizeof(HttpTransfer));
  transfer->request      = *request;
  transfer->result       = result;

  request->headers = NULL;

  transfer->easy = curl_easy_init();
  if (transfer->easy == NULL ||
      curl_easy_setopt(transfer->easy, CURLOPT_WRITEFUNCTION, http_response_write_body) !=
          CURLE_OK ||
      curl_easy_setopt(transfer->easy, CURLOPT_WRITEDATA, &result->response) != CURLE_OK ||
      // The default User-Agent, libcurl omits it if the request headers already set one
      curl_easy_setopt(transfer->easy, CURLOPT_USERAGENT, EXTNAME "/" EXTVERSION) != CURLE_OK) {
    http_result_destroy(end(transfer));
    return NULL;
  }

  return transfer;
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
 *
 * Must be called before the handle is cleaned up. Allocated in the current memory context.
 */
static char *timeout_message(HttpTransfer *transfer) {
  CURL  *easy        = transfer->easy;
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
                  transfer->request.timeout_milliseconds, total * 1000, dns * 1000,
                  handshake * 1000, http * 1000);
}

/*
 * Maps a CURL error to a value of the http.error_type enum
 */
static const char *error_type_for(CURLcode code) {
  if (code == CURLE_OPERATION_TIMEDOUT) return "timeout";

  if (code == CURLE_COULDNT_RESOLVE_PROXY || code == CURLE_COULDNT_RESOLVE_HOST ||
      code == CURLE_COULDNT_CONNECT || code == CURLE_SSL_CONNECT_ERROR)
    return "connection_error";

  return "other";
}

HttpResult *http_transfer_finish(HttpTransfer *transfer, CURLcode code, long status_code) {
  HttpResponse *response = &transfer->result->response;

  // Everything kept in the response lives as long as the result
  MemoryContext old_context = MemoryContextSwitchTo(transfer->result->context);

  if (code != CURLE_OK) {
    ereport(LOG, errmsg(EXTNAME ": request failed: %s", curl_easy_strerror(code)));

    response->error_msg  = code == CURLE_OPERATION_TIMEDOUT ? timeout_message(transfer)
                                                            : pstrdup(curl_easy_strerror(code));
    response->error_type = error_type_for(code);
    resetStringInfo(&response->body);
  } else {
    ereport(LOG, errmsg(EXTNAME ": got response with status %ld and %d body bytes", status_code,
                        response->body.len));
    response->status_code = (int32)status_code;
    http_response_collect_headers(response, transfer->easy);
  }

  MemoryContextSwitchTo(old_context);

  return end(transfer);
}

HttpResult *http_transfer_abort(HttpTransfer *transfer, const char *reason) {
  HttpResponse *response = &transfer->result->response;
  response->error_msg    = MemoryContextStrdup(transfer->result->context, reason);
  response->error_type   = "other";

  return end(transfer);
}
