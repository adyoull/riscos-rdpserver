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
  unsigned step_gap_cs;      /* tests: server_step(0) then a pause this long,
                                as the desktop version's poll loop does */
  int mute_local;            /* silence this computer while a client listens */
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

/* The server thread was lost: forget everything without freeing it. */
void server_abandon (void);
/* Quitting: server_finish, and on RISC OS let go of the module. */
void server_shutdown (void);
/* Why server_start failed, if it knows (else NULL). */
const char *server_start_error (void);
void server_disconnect (void);
/* The client's socket, or -1 (for rdps_cmd_disconnect). */
int server_client_socket (void);
/* Reads the logins file again (after Choices changed it). 0 = done. */
int server_reload_logins (void);
/* Mute this computer while a client listens: on or off, from now. */
void server_set_mute (int on);
void server_get_status (server_status *st);
void server_set_message_hook (void (*hook) (const char *line));

/* Runs until stopped (Escape in the task window) or max_seconds, the
   server in its thread (srvthread.h) resumed every centisecond or so as
   the module's callback does. Returns the exit status. Not in the module. */
int server_run (const server_options *o);

/* Set by the platform to stop the server. */
extern volatile int server_stop;

/* Libxrdp reads the desktop size the client asked for; the server
   replaces it with the screen's (patch 0001 to libxrdp/xrdp_sec.c). */
extern int g_rdps_desktop_width, g_rdps_desktop_height;

#endif
