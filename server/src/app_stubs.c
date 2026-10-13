/* RDPServer: the front end on RISC OS has no server thread (the server is
   the module), but os_calls.c, which keygen.c uses, asks about one.
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <unistd.h>
#include "coro.h"
#include "srvthread.h"

int
co_inside (void)
{
  return 0;
}

int
rdps_thread_ending (void)
{
  return 0;
}

void
rdps_wait_cs (unsigned cs)
{
  if (cs)
    usleep (cs * 10000);
}
