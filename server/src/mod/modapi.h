/* RDPServer module: how the desktop front end talks to it.

   The module has no SWIs, *commands or service call numbers of its own
   (nothing to allocate), like UnixLib's PThreadTicker: the front end
   finds it with OS_Module 18 ("RDPServer"), which gives the module's
   address and its workspace, and reads the table that follows the module
   header at &34. Each entry is called from USR mode as an ordinary
   function (arguments in R0-R3, result in R0, R4-R11 and SP kept) with
   R12 = the workspace; it goes into SVC mode itself, so the module's
   callback can't run in the middle of it.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_MODAPI_H
#define RDPS_MODAPI_H

#include "../server.h"

#define RDPS_MOD_TITLE "RDPServer"
#define RDPS_MOD_MAGIC 0x73504452u        /* "RDPs" */
#define RDPS_MOD_INTERFACE 2
#define RDPS_MOD_TABLE 0x34

/* The entries, in the table's order. */
enum
{
  RDPS_MOD_ATTACH,        /* int (int task): 0, or 1 if another task has it */
  RDPS_MOD_DETACH,        /* void (void): stops the server too */
  RDPS_MOD_SET_FILE,      /* int (const char *path, const char *data, int len):
                             a file libxrdp reads, held in memory; data NULL
                             forgets it */
  RDPS_MOD_START,         /* int (const rdps_mod_options *): 0, or 1 (see
                             the messages) */
  RDPS_MOD_STOP,          /* void (void) */
  RDPS_MOD_DISCONNECT,    /* void (void): drops the client at once */
  RDPS_MOD_RELOAD_LOGINS, /* int (void): after SET_FILE of the logins file */
  RDPS_MOD_STATUS,        /* int (rdps_mod_status *) */
  RDPS_MOD_MESSAGES,      /* int (unsigned *seq, char *buf, int size): the
                             lines after *seq, each "<tag> <text>\n" (tag s:
                             the server's, x: libxrdp's log); returns the
                             bytes written, updates *seq */
  RDPS_MOD_SET_MUTE,      /* int (int on): mute this computer while a client
                             listens, from now (0.2-test11) */
  RDPS_MOD_ENTRIES
};
/* The same as a number, for the front end's interface tag (modclient.c) */
#define RDPS_MOD_ENTRIES_N 10
typedef char rdps_mod_entries_n_check[RDPS_MOD_ENTRIES == RDPS_MOD_ENTRIES_N ? 1 : -1];

typedef struct
{
  unsigned magic, version, entries;
  unsigned offset[RDPS_MOD_ENTRIES];      /* from the module's start */
} rdps_mod_table;

/* What RDPS_MOD_START needs (copied: the front end's memory is only
   there while it's paged in). */
typedef struct
{
  char port[8];
  char address[64];
  char ini[256];            /* path of xrdp.ini, as given to SET_FILE */
  char logins[256];         /* path of the logins file, likewise */
  unsigned interval_cs, idle_interval_cs, handshake_cs;
  int rows_per_scan;
  int verbose;
  int mute_local;           /* silence this computer while a client listens */
} rdps_mod_options;

typedef struct
{
  server_status st;
  int errorbox_open;        /* a Wimp error box is open */
  unsigned callbacks;       /* times the callback ran the server */
  unsigned errorbox_steps;  /* times it ran from inside an error box */
  unsigned heap_bytes;      /* the module's dynamic area */
  int dead;                 /* the server stopped for good (see the log) */
} rdps_mod_status;

#endif
