/* RDPSProbe: the probe itself, apart from main (so the fake test can run it).

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef PROBE_PROBE_H
#define PROBE_PROBE_H

#include <stdio.h>

#define PROBE_VERSION "0.1"

typedef struct
{
  unsigned min_cs;       /* time each test for at least this long */
  unsigned max_passes;   /* and never more passes than this */
  int pointer;           /* also try moving the mouse */
} probe_options;

/* Runs every test, writing the report to out. Returns 0 if nothing failed. */
int probe_run (FILE *out, const probe_options *o);

#endif
