/* RDPServer: command line.

     RDPServer [-p port] [-a address] [-i centiseconds] [-r rows]
               [-t seconds] [-v]
     RDPServer -setlogin NAME PASSWORD
     RDPServer -clearlogin

   On RISC OS this is the desktop front end: the server itself is the
   RDPServer module (src/mod), which !Run loads; -p and -a don't apply
   there (Choices has the port). The Linux builds run the server here, in
   its thread (srvthread.h), for the tests.
   -p  port to listen on (3389; the desktop version uses Choices)
   -a  address to listen on (every interface)
   -i  time a pass over the screen takes, centiseconds (5; 10 reading
       every row)
   -r  the most rows of the screen read a turn (0: no limit; each turn
       reads the rows due since the last)
   -s  read 1 row in this many each pass (1, 2, 4, 8 or 16; 8), and the
       rest of a tile where one changed. The Linux builds only: on RISC
       OS, *Set RDPServer$SampleRows
   -t  stop after this many seconds (for tests)
   -g  pause this many centiseconds after each step, as the desktop
       version's poll loop does (for tests)
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
#ifdef __riscos
#include <kernel.h>
#include <swis.h>
#include "wimp.h"
#endif

/* from the Makefile (VERSION and TESTSUFFIX), so it can't fall behind */
#ifndef RDPS_VERSION
#define RDPS_VERSION "dev"
#endif
#define VERSION RDPS_VERSION

#ifndef __riscos
static void
on_signal (int sig)
{
  if (sig == SIGUSR2)
    server_prereset = 1;
  else if (sig == SIGUSR1)
    server_shutdown_seen = 1;
  else
    server_stop = 1;
}
#endif

static int
usage (void)
{
  fprintf (stderr, "Usage: RDPServer [-p port] [-a address] [-i centiseconds] "
           "[-r rows] [-s 1|2|4|8|16] [-t seconds] [-sound] [-m] [-tls] [-v]\n"
           "       (-sound: send this computer's sound to the client;\n"
           "        -m: mute this computer while a client listens;\n"
           "        -tls: refuse clients that don't use TLS)\n"
           "       RDPServer -setlogin NAME PASSWORD\n"
           "       RDPServer -clearlogin\n");
  return 2;
}

int
main (int argc, char **argv)
{
  server_options o;
  char keys[512], ini[512], logins[512], tls_key[512], tls_cert[512];
  int i;
#ifdef __riscos
  char settings[512], logfile[512], xrdplog[512];
#endif

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
  o.interval_cs = 0;       /* the server's default */
  for (i = 1; i < argc; i++)
    {
      const char *a = argv[i];
      const char *v = i + 1 < argc ? argv[i + 1] : NULL;
      if (strcmp (a, "-v") == 0)
        o.verbose = 1;
      else if (strcmp (a, "-m") == 0)
        o.mute_local = 1;
      else if (strcmp (a, "-tls") == 0)
        o.require_tls = 1;
      else if (strcmp (a, "-sound") == 0)
        o.send_sound = 1;
      else if (v && strcmp (a, "-p") == 0)
        o.port = argv[++i];
      else if (v && strcmp (a, "-a") == 0)
        o.address = argv[++i];
      else if (v && strcmp (a, "-i") == 0)
        o.interval_cs = (unsigned) atoi (argv[++i]);
      else if (v && strcmp (a, "-r") == 0)
        o.rows_per_scan = atoi (argv[++i]);
      else if (v && strcmp (a, "-s") == 0)
        o.sample_rows = atoi (argv[++i]);
      else if (v && strcmp (a, "-g") == 0)
        o.step_gap_cs = (unsigned) atoi (argv[++i]);
      else if (v && strcmp (a, "-t") == 0)
        o.max_seconds = atoi (argv[++i]);
      else if (v && strcmp (a, "-silent") == 0)
        o.silent_cs = (unsigned) atoi (argv[++i]);
      else if (v && strcmp (a, "-stall") == 0)
        o.stall_cs = (unsigned) atoi (argv[++i]);
      else
        return usage ();
    }

  snprintf (ini, sizeof ini, "%s/xrdp.ini", plat_resources_dir ());
  o.ini = ini;
  o.logins = logins;
  snprintf (keys, sizeof keys, "%s/rsakeys.ini", plat_choices_dir ());
  snprintf (tls_key, sizeof tls_key, "%s/key.pem", plat_choices_dir ());
  snprintf (tls_cert, sizeof tls_cert, "%s/cert.pem", plat_choices_dir ());
#ifdef __riscos
    {
      /* The desktop front end. The server's messages go to Choices (Log,
         and libxrdp's to XrdpLog) and the Status window. */
      wimp_setup w;
      snprintf (settings, sizeof settings, "%s/Settings", plat_choices_dir ());
      snprintf (logfile, sizeof logfile, "%s/Log", plat_choices_dir ());
      snprintf (xrdplog, sizeof xrdplog, "%s/XrdpLog", plat_choices_dir ());
      o.log_file = xrdplog;
      if (access (keys, R_OK) != 0)
        {
          /* Once only; it can take a minute. */
          _swix (Hourglass_On, 0);
          i = rdps_keygen (keys);
          _swix (Hourglass_Off, 0);
          if (i != 0)
            {
              wimp_report ("RDPServer couldn't make or save its key in Choices.");
              return 1;
            }
        }
      /* TLS's key and certificate, from that key (quick). Without them
         clients get Standard RDP Security only. */
      if (access (tls_key, R_OK) != 0 || access (tls_cert, R_OK) != 0)
        rdps_make_tls_cert (keys, tls_key, tls_cert);
      signal (SIGPIPE, SIG_IGN);
      w.opt = &o;
      w.version = VERSION;
      w.settings_file = settings;
      w.logins_file = logins;
      w.log_file = logfile;
      return wimp_run (&w);
    }
#else

  printf ("RDPServer " VERSION ": use it on your own network.\n");

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
  if ((access (tls_key, R_OK) != 0 || access (tls_cert, R_OK) != 0)
      && rdps_make_tls_cert (keys, tls_key, tls_cert) != 0)
    fprintf (stderr, "RDPServer: couldn't make the TLS certificate: Standard RDP "
             "Security only\n");

  signal (SIGINT, on_signal);
  signal (SIGTERM, on_signal);
  signal (SIGUSR2, on_signal);          /* a pretend reset (tests) */
  signal (SIGUSR1, on_signal);          /* a pretend desktop shutdown (tests) */
  /* A client that resets mid-send must not kill the server. */
  signal (SIGPIPE, SIG_IGN);
  return server_run (&o);
#endif
}
