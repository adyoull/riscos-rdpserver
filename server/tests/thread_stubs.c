/* RDPServer unit tests: what os_calls.c needs from the server thread and
   the platform, without the server.
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <sys/time.h>
#include <unistd.h>
#include "srvthread.h"
#include "platform.h"

unsigned
plat_time_cs (void)
{
  struct timeval tv;
  gettimeofday (&tv, 0);
  return (unsigned) (tv.tv_sec * 100 + tv.tv_usec / 10000);
}

int
rdps_thread_ending (void)
{
  return 0;
}

void
rdps_wait_cs (unsigned cs)
{
  usleep (cs * 10000);
}
