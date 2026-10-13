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
  unsigned idle_interval_cs; /* scan period when nothing's happening (25) */
  unsigned handshake_cs;     /* time a client has to finish connecting (3000) */
  const char *log_file;      /* libxrdp's messages here, not the console */
} server_options;

typedef struct
{
  int listening, connecting, connected, login_set;
  char client[64], user[64];
  int width, height, bpp;
  unsigned tiles_sent;
} server_status;

/* The pieces of server_run, for a front end with its own loop (the Wimp):
   server_start, then server_step(0) whenever it gets control (returns
   the centiseconds until it next needs to run), server_finish. */
int server_start (const server_options *o);
int server_step (unsigned max_wait_cs);
void server_finish (void);
void server_disconnect (void);
/* Reads the logins file again (after Choices changed it). 0 = done. */
int server_reload_logins (void);
void server_get_status (server_status *st);
void server_set_message_hook (void (*hook) (const char *line));

/* Runs until stopped (Escape in the task window) or max_seconds. Returns
   the exit status. */
int server_run (const server_options *o);

/* Set by the platform to stop the server. */
extern volatile int server_stop;

/* Libxrdp reads the desktop size the client asked for; the server
   replaces it with the screen's (patch 0001 to libxrdp/xrdp_sec.c). */
extern int g_rdps_desktop_width, g_rdps_desktop_height;

#endif
