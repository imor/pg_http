#include "transfer.h"

#include <float.h>
#include <string.h>

static size_t write_callback(char *data, size_t size, size_t nmemb, void *userp) {
  HttpResponse *response = (HttpResponse *)userp;
  size_t        total    = size * nmemb;

  appendBinaryStringInfo(&response->body, data, (int)total);

  return total;
}

HttpTransfer *http_transfer_create(HttpRequest *request, Oid requests_relid) {
  // The Internal variant avoids the GNU statement expression in the macro
  MemoryContext context =
      AllocSetContextCreateInternal(TopMemoryContext, EXTNAME " transfer", ALLOCSET_SMALL_MINSIZE,
                                    ALLOCSET_SMALL_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE);
  MemoryContext old_context = MemoryContextSwitchTo(context);

  HttpTransfer *transfer   = palloc(sizeof(HttpTransfer));
  transfer->context        = context;
  transfer->request        = http_request_take(request);
  transfer->requests_relid = requests_relid;
  http_response_init(&transfer->response);

  MemoryContextSwitchTo(old_context);

  transfer->easy = curl_easy_init();
  if (transfer->easy == NULL ||
      curl_easy_setopt(transfer->easy, CURLOPT_WRITEFUNCTION, write_callback) != CURLE_OK ||
      curl_easy_setopt(transfer->easy, CURLOPT_WRITEDATA, &transfer->response) != CURLE_OK ||
      // The default User-Agent, libcurl omits it if the request headers already set one
      curl_easy_setopt(transfer->easy, CURLOPT_USERAGENT, EXTNAME "/" EXTVERSION) != CURLE_OK) {
    http_transfer_destroy(transfer);
    return NULL;
  }

  return transfer;
}

void http_transfer_release_handle(HttpTransfer *transfer) {
  if (transfer->easy == NULL) return;
  curl_easy_cleanup(transfer->easy);
  transfer->easy = NULL;

  // Only free once libcurl is done with the headers
  curl_slist_free_all(transfer->request.headers);
  transfer->request.headers = NULL;
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
char *http_transfer_timeout_message(HttpTransfer *transfer) {
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

void http_transfer_destroy(HttpTransfer *transfer) {
  http_transfer_release_handle(transfer);
  MemoryContextDelete(transfer->context);
}
