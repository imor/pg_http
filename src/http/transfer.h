#ifndef PG_HTTP_TRANSFER_H
#define PG_HTTP_TRANSFER_H

#include "pg_prelude.h"
#include "request.h"
#include "response.h"

#include <curl/curl.h>

/*
 * An HTTP transfer: a request and the response it gets, along with the state of sending it.
 * Everything it owns is allocated in its own memory context, so it stays valid until the
 * transfer is destroyed, which is after its response was saved.
 */
typedef struct HttpTransfer {
  // Holds the memory of the transfer, including the request and the response data
  MemoryContext context;

  // The request, owned by the transfer. Its headers are freed with the handle.
  HttpRequest request;

  // The response of the request
  HttpResponse response;

  // CURL's easy handle for this HTTP transfer. NULL once released.
  CURL *easy;

  // The oid of the requests table when the request was read from it. Used to drop the response
  // if the extension was dropped, and maybe created again, in the meantime.
  Oid requests_relid;
} HttpTransfer;

/*
 * Creates a transfer for `request`, which it takes over without copying: the request's context
 * becomes the transfer's and is moved under TopMemoryContext, and the ownership of the headers
 * moves to the transfer. `request` is left without a context and headers. Returns NULL if the
 * CURL handle couldn't be created.
 */
HttpTransfer *http_transfer_create(HttpRequest *request, Oid requests_relid);

/*
 * Release the CURL easy handle, keeping the request and the response data. Must only be
 * called once the transfer is finished and removed from the multi handle.
 */
void http_transfer_release_handle(HttpTransfer *transfer);

/*
 * Returns a message with the details of a timed out transfer: the time spent in each step.
 * Allocated in the current memory context. Must be called before the handle is released.
 */
char *http_transfer_timeout_message(HttpTransfer *transfer);

/*
 * Destroy an HttpTransfer, freeing the request and the response with it
 */
void http_transfer_destroy(HttpTransfer *transfer);

#endif /* PG_HTTP_TRANSFER_H */
