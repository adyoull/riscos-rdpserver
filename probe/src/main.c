/* RDPSProbe: command line.
     RDPSProbe [-p] [-t centiseconds] [-o file]
   -p  also move the mouse (and put it back)
   -t  how long to time each test (default 200 cs)
   -o  write the report to a file as well as the screen

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "probe.h"

int main (int argc, char **argv)
{
  probe_options o;
  const char *file = NULL;
  int i, rc;

  o.min_cs = 200;
  o.max_passes = 100000;
  o.pointer = 0;
  for (i = 1; i < argc; i++)
    {
      if (strcmp (argv[i], "-p") == 0)
        o.pointer = 1;
      else if (strcmp (argv[i], "-t") == 0 && i + 1 < argc)
        o.min_cs = (unsigned) strtoul (argv[++i], NULL, 10);
      else if (strcmp (argv[i], "-o") == 0 && i + 1 < argc)
        file = argv[++i];
      else
        {
          fprintf (stderr, "Usage: RDPSProbe [-p] [-t centiseconds] [-o file]\n");
          return 2;
        }
    }
  if (o.min_cs == 0)
    o.min_cs = 1;

  if (!file)
    return probe_run (stdout, &o);

  /* Write the report to the file, then show it. */
  {
    FILE *f = fopen (file, "w+");
    int c;
    if (!f)
      {
        fprintf (stderr, "Can't write %s\n", file);
        return 1;
      }
    rc = probe_run (f, &o);
    rewind (f);
    while ((c = getc (f)) != EOF)
      putchar (c);
    fclose (f);
  }
  return rc;
}
