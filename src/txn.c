#include "txn.h"

void run_in_transaction(txn_fn fn, void *arg, MemoryContext spi_context) {
  SetCurrentStatementStartTimestamp();
  StartTransactionCommand();
  PushActiveSnapshot(GetTransactionSnapshot());

  MemoryContext old_context = CurrentMemoryContext;
  if (spi_context != NULL) MemoryContextSwitchTo(spi_context);

  int ret_code = SPI_connect();
  if (ret_code != SPI_OK_CONNECT)
    ereport(ERROR, errmsg(EXTNAME ": SPI_connect failed: %s", SPI_result_code_string(ret_code)));

  fn(arg);

  ret_code = SPI_finish();
  if (ret_code != SPI_OK_FINISH)
    ereport(ERROR, errmsg(EXTNAME ": SPI_finish failed: %s", SPI_result_code_string(ret_code)));

  // SPI_finish returns to the context SPI_connect was called in
  MemoryContextSwitchTo(old_context);

  PopActiveSnapshot();
  CommitTransactionCommand();

  // Background workers must flush their pending pgstat counters themselves, otherwise the
  // counters of their writes never reach the shared stats and autovacuum doesn't see them.
  pgstat_report_stat(false);
}
