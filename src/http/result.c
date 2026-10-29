#include "result.h"

HttpResult *http_result_create(HttpRequest *request, Oid requests_relid) {
  MemoryContext context = request->context;
  MemoryContextSetParent(context, TopMemoryContext);
  MemoryContext old_context = MemoryContextSwitchTo(context);

  HttpResult *result     = palloc(sizeof(HttpResult));
  result->context        = context;
  result->id             = request->id;
  result->requests_relid = requests_relid;
  http_response_init(&result->response);

  MemoryContextSwitchTo(old_context);

  request->context = NULL;

  return result;
}

HttpResult *http_result_rejected(HttpRequest *request, Oid requests_relid, const char *reason) {
  HttpResult *result = http_result_create(request, requests_relid);

  curl_slist_free_all(request->headers);
  request->headers = NULL;

  result->response.error_msg  = MemoryContextStrdup(result->context, reason);
  result->response.error_type = "other";

  return result;
}

void http_result_destroy(HttpResult *result) {
  MemoryContextDelete(result->context);
}
