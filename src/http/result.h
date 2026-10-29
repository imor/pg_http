#ifndef PG_HTTP_RESULT_H
#define PG_HTTP_RESULT_H

#include "pg_prelude.h"
#include "request.h"
#include "response.h"

/*
 * The result of a request: its response or why it failed, waiting to be saved in the responses
 * table. Everything it owns is allocated in its own memory context, child of TopMemoryContext.
 */
typedef struct HttpResult {
  // Holds the memory of the result, including the request it was created from
  MemoryContext context;

  // The id of the request in the queue table
  int64 id;

  // The oid of the requests table when the request was read from it. Used to drop the result if
  // the extension was dropped, and maybe created again, in the meantime.
  Oid requests_relid;

  // The response of the request
  HttpResponse response;
} HttpResult;

/*
 * Creates an empty result for `request`, taking over its context, which is moved under
 * TopMemoryContext, so its strings stay valid until the result is destroyed. `request` is left
 * without a context. Its headers are not touched.
 */
HttpResult *http_result_create(HttpRequest *request, Oid requests_relid);

/*
 * Creates the result of a request which isn't sent, failing with `reason`. Takes over `request`
 * like http_result_create and frees its headers.
 */
HttpResult *http_result_rejected(HttpRequest *request, Oid requests_relid, const char *reason);

/*
 * Destroys a result, freeing all its memory
 */
void http_result_destroy(HttpResult *result);

#endif /* PG_HTTP_RESULT_H */
