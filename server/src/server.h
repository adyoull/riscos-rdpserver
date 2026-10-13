/* RDPServer: the server (one client at a time).
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_SERVER_H
#define RDPS_SERVER_H

typedef struct
{
  const char *port;          /* "3389" */
  const char *address;       /* "" for every interface */
  const char *ini;           /* the xrdp.ini libxrdp reads */
  const char *logins;        /* the logins file (see auth.h) */
  unsigned interval_cs;      /* time between screen scans */
  int rows_per_scan;         /* rows read each scan (0: the whole screen) */
  int max_seconds;           /* stop after this long (0: never; for tests) */
  int verbose;
} server_options;

/* Runs until stopped (Escape in the task window) or max_seconds. Returns
   the exit status. */
int server_run (const server_options *o);

/* Set by the platform to stop the server. */
extern volatile int server_stop;

/* Libxrdp reads the desktop size the client asked for; the server
   replaces it with the screen's (patch 0001 to libxrdp/xrdp_sec.c). */
extern int g_rdps_desktop_width, g_rdps_desktop_height;

#endif
