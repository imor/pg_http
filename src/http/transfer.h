#ifndef PG_HTTP_TRANSFER_H
#define PG_HTTP_TRANSFER_H

#include "pg_prelude.h"
#include "request.h"
#include "result.h"

#include <curl/curl.h>

/*
 * An HTTP transfer in flight: a request being sent and the result its response is collected
 * in. A transfer ends with http_transfer_finish or http_transfer_abort, which free it and hand
 * over its result.
 */
typedef struct HttpTransfer {
  // The request, its strings are allocated in the result's context. Its headers are owned by
  // the transfer, libcurl uses them until the transfer ends.
  HttpRequest request;

  // CURL's easy handle for this HTTP transfer
  CURL *easy;

  // The result the response is collected in
  HttpResult *result;
} HttpTransfer;

/*
 * Creates a transfer for `request`, taking over its context and headers, see http_result_create.
 * Returns NULL if the CURL handle couldn't be created, `request` is freed then.
 */
HttpTransfer *http_transfer_create(HttpRequest *request, Oid requests_relid);

/*
 * Ends a transfer which completed with `code` and, if successful, `status_code`: fills in its
 * result from the response, frees the transfer and returns the result. The handle must have been
 * removed from the multi handle.
 */
HttpResult *http_transfer_finish(HttpTransfer *transfer, CURLcode code, long status_code);

/*
 * Ends a transfer which couldn't be started, failing with `reason`: frees the transfer and
 * returns its result. The handle must not be in a multi handle.
 */
HttpResult *http_transfer_abort(HttpTransfer *transfer, const char *reason);

#endif /* PG_HTTP_TRANSFER_H */
