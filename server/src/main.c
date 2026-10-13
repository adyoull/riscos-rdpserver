/* RDPServer: command line.

     RDPServer [-p port] [-a address] [-i centiseconds] [-r rows]
               [-t seconds] [-v]
     RDPServer -setlogin NAME PASSWORD
     RDPServer -clearlogin

   -p  port to listen on (3389)
   -a  address to listen on (every interface)
   -i  time between screen scans, centiseconds (10)
   -r  rows of the screen read each scan (0, the default: all of them)
   -t  stop after this many seconds (for tests)
   -v  say more (each input event)
   -setlogin    set the one user name and password clients must give
   -clearlogin  let anyone connect again

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "platform.h"
#include "keygen.h"
#include "auth.h"
#include "server.h"

#define VERSION "0.1-test2"

static void
on_signal (int sig)
{
  (void) sig;
  server_stop = 1;
}

static int
usage (void)
{
  fprintf (stderr, "Usage: RDPServer [-p port] [-a address] [-i centiseconds] "
           "[-r rows] [-t seconds] [-v]\n"
           "       RDPServer -setlogin NAME PASSWORD\n"
           "       RDPServer -clearlogin\n");
  return 2;
}

int
main (int argc, char **argv)
{
  server_options o;
  char keys[512], ini[512], logins[512];
  int i;

  snprintf (logins, sizeof logins, "%s/logins", plat_choices_dir ());
  if (argc == 4 && strcmp (argv[1], "-setlogin") == 0)
    {
      if (auth_set (logins, argv[2], argv[3]) != 0)
        {
          fprintf (stderr, "RDPServer: couldn't save the login (a name has no "
                   "spaces and is under 64 characters)\n");
          return 1;
        }
      printf ("RDPServer: clients must now log in as %s.\n", argv[2]);
      return 0;
    }
  if (argc == 2 && strcmp (argv[1], "-clearlogin") == 0)
    {
      remove (logins);
      printf ("RDPServer: no login now: anyone can connect.\n");
      return 0;
    }

  memset (&o, 0, sizeof o);
  o.port = "3389";
  o.address = "";
  o.interval_cs = 10;
  for (i = 1; i < argc; i++)
    {
      const char *a = argv[i];
      const char *v = i + 1 < argc ? argv[i + 1] : NULL;
      if (strcmp (a, "-v") == 0)
        o.verbose = 1;
      else if (v && strcmp (a, "-p") == 0)
        o.port = argv[++i];
      else if (v && strcmp (a, "-a") == 0)
        o.address = argv[++i];
      else if (v && strcmp (a, "-i") == 0)
        o.interval_cs = (unsigned) atoi (argv[++i]);
      else if (v && strcmp (a, "-r") == 0)
        o.rows_per_scan = atoi (argv[++i]);
      else if (v && strcmp (a, "-t") == 0)
        o.max_seconds = atoi (argv[++i]);
      else
        return usage ();
    }
  if (o.interval_cs == 0)
    o.interval_cs = 1;

  printf ("RDPServer " VERSION ": a test build. Standard RDP Security only "
          "(RC4, no TLS): use it on your own network.\n");

  snprintf (ini, sizeof ini, "%s/xrdp.ini", plat_resources_dir ());
  o.ini = ini;
  o.logins = logins;
  snprintf (keys, sizeof keys, "%s/rsakeys.ini", plat_choices_dir ());
  if (access (keys, R_OK) != 0)
    {
      printf ("Making this server's key (once only; it can take a minute)...\n");
      fflush (stdout);
      if (rdps_keygen (keys) != 0)
        {
          fprintf (stderr, "RDPServer: couldn't make or save the key (%s)\n", keys);
          return 1;
        }
      printf ("Done.\n");
    }

  signal (SIGINT, on_signal);
  signal (SIGTERM, on_signal);
  /* A client that resets mid-send must not kill the server. */
  signal (SIGPIPE, SIG_IGN);
  return server_run (&o);
}
