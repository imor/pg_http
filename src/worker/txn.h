#ifndef PG_HTTP_TXN_H
#define PG_HTTP_TXN_H

#include "pg_prelude.h"

typedef void (*txn_fn)(void *);

/*
 * Runs `fn` inside a transaction with an SPI connection and an active snapshot.
 *
 * SPI_palloc allocates in the context current at SPI_connect. If `spi_context`
 * is not NULL the connection is made in it, so that memory allocated by SPI
 * (e.g. results copied out by `fn`) survives the commit.
 */
void run_in_transaction(txn_fn fn, void *arg, MemoryContext spi_context);

#endif /* PG_HTTP_TXN_H */
