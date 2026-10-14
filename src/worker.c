#include "worker.h"

/*
 * The main entry point for the pg_http background worker.
 */
void pg_http_main(__attribute__((unused)) Datum main_arg)
{
    while (true)
    {
        CHECK_FOR_INTERRUPTS();
        pg_usleep(1000000);
    }
}