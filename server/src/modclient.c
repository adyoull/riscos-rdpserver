/* RDPServer: the front end's side of the module (RISC OS only).

   Since 0.2 the server runs in the RDPServer module (src/mod), so it goes
   on while the desktop doesn't multitask (a Wimp error box, a program
   that holds the machine). The front end (wimp.c) still calls the
   server_* functions of server.h; here they are calls to the module,
   found with OS_Module 18 and called through the table after its header
   (mod/modapi.h). No SWIs.

   The files libxrdp reads (xrdp.ini, rsakeys.ini, logins) are read here
   and given to the module, which never touches FileSwitch. Its messages
   come back on each server_step: the server's go to the hook (the Status
   window and Log), libxrdp's to the options' log_file (XrdpLog).

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>

#include "mod/modapi.h"
#include "platform.h"
#include "server.h"

/* modcall.s: calls fn (a, b, c) with R12 = ws. */
extern int rdps_modcall (const void *fn, void *ws, int a, int b, int c);

static const char *mod_base;
static void *mod_ws;
static const rdps_mod_table *table;
static int attached;
static void (*hook) (const char *line);
static unsigned seq;
static const server_options *opts;
static FILE *xrdp_log;
static char start_error[160];

static int
call (int entry, int a, int b, int c)
{
  return rdps_modcall (mod_base + table->offset[entry], mod_ws, a, b, c);
}

static void
say (const char *text)
{
  if (hook)
    hook (text);
}

/* Finds the module and checks it's the one this front end knows. */
static int
find (void)
{
  const char *base = NULL;
  void *ws = NULL;
  const rdps_mod_table *t;

  if (table)
    return 0;
  if (_swix (OS_Module, _INR (0, 1) | _OUTR (3, 4), 18, RDPS_MOD_TITLE, &base, &ws)
      || !base || !ws)
    {
      snprintf (start_error, sizeof start_error,
                "The RDPServer module isn't loaded (!RDPServer's !Run loads it).");
      return -1;
    }
  t = (const rdps_mod_table *) (base + RDPS_MOD_TABLE);
  if (t->magic != RDPS_MOD_MAGIC || t->version != RDPS_MOD_INTERFACE
      || t->entries < RDPS_MOD_ENTRIES)
    {
      snprintf (start_error, sizeof start_error,
                "The RDPServer module loaded is a different version: quit, "
                "*RMKill RDPServer, and run !RDPServer again.");
      return -1;
    }
  mod_base = base;
  mod_ws = ws;
  table = t;
  return 0;
}

/* Reads a whole file; NULL if it can't. */
static char *
slurp (const char *path, int *len)
{
  FILE *f = fopen (path, "rb");
  char *d;
  long n;
  *len = 0;
  if (!f)
    return NULL;
  if (fseek (f, 0, SEEK_END) != 0 || (n = ftell (f)) < 0 || fseek (f, 0, SEEK_SET) != 0)
    {
      fclose (f);
      return NULL;
    }
  d = malloc ((size_t) n + 1);
  if (d && fread (d, 1, (size_t) n, f) != (size_t) n)
    {
      free (d);
      d = NULL;
    }
  fclose (f);
  if (d)
    {
      d[n] = 0;
      *len = (int) n;
    }
  return d;
}

/* Gives the module a file under the name libxrdp will open it by (no
   file: an empty one, which for logins means "anyone"). */
static int
give (const char *path, int required)
{
  int len, r;
  char *d = slurp (path, &len);
  if (!d && required)
    return -1;
  r = call (RDPS_MOD_SET_FILE, (int) path, (int) (d ? d : ""), d ? len : 0);
  free (d);
  return r;
}

static void
copy (char *to, size_t size, const char *from)
{
  snprintf (to, size, "%s", from ? from : "");
}

void
server_set_message_hook (void (*h) (const char *line))
{
  hook = h;
}

const char *
server_start_error (void)
{
  return start_error[0] ? start_error : NULL;
}

static void
detach_at_exit (void)
{
  if (attached && table)
    call (RDPS_MOD_DETACH, 0, 0, 0);
  attached = 0;
}

int
server_start (const server_options *o)
{
  rdps_mod_options mo;
  char keys[300];
  int task = 0;

  start_error[0] = 0;
  if (find () != 0)
    {
      say (start_error);
      return 1;
    }
  if (!attached)
    {
      _swix (Wimp_ReadSysInfo, _IN (0) | _OUT (0), 5, &task);
      if (call (RDPS_MOD_ATTACH, task, 0, 0) != 0)
        {
          snprintf (start_error, sizeof start_error,
                    "Another !RDPServer is using the RDPServer module.");
          say (start_error);
          return 1;
        }
      attached = 1;
      atexit (detach_at_exit);
    }
  opts = o;
  if (o->log_file && !xrdp_log)
    xrdp_log = fopen (o->log_file, "w");
  snprintf (keys, sizeof keys, "%s/rsakeys.ini", plat_choices_dir ());
  if (give (o->ini, 1) != 0 || give (keys, 1) != 0 || give (o->logins, 0) != 0)
    {
      snprintf (start_error, sizeof start_error,
                "RDPServer couldn't read its xrdp.ini or its key, or the module "
                "had no room for them.");
      say (start_error);
      return 1;
    }
  memset (&mo, 0, sizeof mo);
  copy (mo.port, sizeof mo.port, o->port);
  copy (mo.address, sizeof mo.address, o->address);
  copy (mo.ini, sizeof mo.ini, o->ini);
  copy (mo.logins, sizeof mo.logins, o->logins);
  mo.interval_cs = o->interval_cs;
  mo.idle_interval_cs = o->idle_interval_cs;
  mo.handshake_cs = o->handshake_cs;
  mo.rows_per_scan = o->rows_per_scan;
  mo.verbose = o->verbose;
  mo.mute_local = o->mute_local;
  if (call (RDPS_MOD_START, (int) &mo, 0, 0) != 0)
    {
      server_step (0);   /* its messages say why */
      return 1;
    }
  return 0;
}

/* The front end's turn: the module's messages, and when to look again. */
int
server_step (unsigned max_wait_cs)
{
  static char buf[8192];
  int n;
  server_status st;
  (void) max_wait_cs;
  if (!table)
    return 25;
  while ((n = call (RDPS_MOD_MESSAGES, (int) &seq, (int) buf, (int) sizeof buf)) > 0)
    {
      char *line = buf, *nl;
      while ((nl = strchr (line, '\n')) != NULL)
        {
          *nl = 0;
          if (line[0] == 's' && line[1] == ' ')
            say (line + 2);
          else if (line[0] == 'x' && line[1] == ' ' && xrdp_log)
            {
              fprintf (xrdp_log, "%s\n", line + 2);
              fflush (xrdp_log);
            }
          line = nl + 1;
        }
      if (n < (int) sizeof buf / 2)
        break;
    }
  server_get_status (&st);
  return st.connected || st.connecting ? 10 : 25;
}

void
server_finish (void)
{
  if (table && attached)
    {
      call (RDPS_MOD_STOP, 0, 0, 0);
      server_step (0);
    }
}

void
server_disconnect (void)
{
  if (table && attached)
    call (RDPS_MOD_DISCONNECT, 0, 0, 0);
}

int
server_reload_logins (void)
{
  if (!table || !attached || !opts)
    return -1;
  if (give (opts->logins, 0) != 0)
    return -1;
  return call (RDPS_MOD_RELOAD_LOGINS, 0, 0, 0);
}

void
server_set_mute (int on)
{
  if (table && attached)
    call (RDPS_MOD_SET_MUTE, on, 0, 0);
}

void
server_get_status (server_status *st)
{
  static rdps_mod_status ms;
  memset (st, 0, sizeof *st);
  if (!table)
    return;
  call (RDPS_MOD_STATUS, (int) &ms, 0, 0);
  *st = ms.st;
}

/* Quitting: stop, let go of the module, and remove it, so the next run
   loads the module from the application (a new version included). */
void
server_shutdown (void)
{
  if (!table)
    return;
  if (attached)
    {
      call (RDPS_MOD_STOP, 0, 0, 0);
      server_step (0);
      call (RDPS_MOD_DETACH, 0, 0, 0);
      attached = 0;
    }
  table = NULL;
  _swix (OS_Module, _INR (0, 1), 4, RDPS_MOD_TITLE);
  if (xrdp_log)
    fclose (xrdp_log);
  xrdp_log = NULL;
}
