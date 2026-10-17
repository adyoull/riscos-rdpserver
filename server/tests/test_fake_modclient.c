/* RDPServer: the front end's side of the module (src/modclient.c) against
   the fake RISC OS, with a pretend module: its table's entries are
   functions here (32-bit, so offsets from the "module" reach them).
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include "fake.h"
#include "swis.h"
#include "mod/modapi.h"
#include "platform.h"
#include "server.h"

static char dir[64];
const char *plat_choices_dir (void) { return dir; }
const char *plat_resources_dir (void) { return dir; }

/* --- the pretend module --------------------------------------------------- */
static struct
{
  unsigned header[13];
  rdps_mod_table table;
} module;
static int ws_word;
static int loaded = 1, attach_answer, start_answer, connected;
static int calls[RDPS_MOD_ENTRIES];
static void *last_ws;
static int attached_task;
static char files[8][300], file_data[8][300];
static int file_null[8];
static int nfiles;
static rdps_mod_options started;
static const char *pending_messages;
static int killed;
static char killed_name[32];

static int m_attach (int task, int b, int c) { (void) b; (void) c; attached_task = task; return attach_answer; }
static int detach_arg = -1;
static int m_detach (int a, int b, int c) { (void) b; (void) c; detach_arg = a; return 0; }
static int m_set_file (int path, int data, int len)
{
  if (nfiles < 8)
    {
      snprintf (files[nfiles], sizeof files[0], "%s", (const char *) (intptr_t) path);
      file_null[nfiles] = data == 0;
      snprintf (file_data[nfiles], sizeof file_data[0], "%.*s", data ? len : 0,
                data ? (const char *) (intptr_t) data : "");
    }
  nfiles++;
  return 0;
}
static int m_start (int o, int b, int c)
{
  (void) b; (void) c;
  started = *(const rdps_mod_options *) (intptr_t) o;
  if (start_answer)
    pending_messages = "s can't listen on port 80: Address already in use\n";
  return start_answer;
}
static int m_stop (int a, int b, int c) { (void) a; (void) b; (void) c; return 0; }
static int m_disconnect (int a, int b, int c) { (void) a; (void) b; (void) c; connected = 0; return 0; }
static int m_reload (int a, int b, int c) { (void) a; (void) b; (void) c; return 0; }
static int m_status (int s, int b, int c)
{
  rdps_mod_status *ms = (rdps_mod_status *) (intptr_t) s;
  (void) b; (void) c;
  memset (ms, 0, sizeof *ms);
  ms->st.listening = 1;
  ms->st.connected = connected;
  ms->st.width = 1920;
  snprintf (ms->st.user, sizeof ms->st.user, "andrew");
  return 0;
}
static int mute_arg = -1;
static int m_set_mute (int on, int b, int c) { (void) b; (void) c; mute_arg = on; return 0; }
static int tls_arg = -1;
static int m_set_tls (int on, int b, int c) { (void) b; (void) c; tls_arg = on; return 0; }
static int sound_arg = -1;
static int m_set_sound (int on, int b, int c) { (void) b; (void) c; sound_arg = on; return 0; }
static int verbose_arg = -1;
static int m_set_verbose (int on, int b, int c) { (void) b; (void) c; verbose_arg = on; return 0; }
static int m_messages (int seqp, int buf, int size)
{
  unsigned *seq = (unsigned *) (intptr_t) seqp;
  int n;
  if (!pending_messages)
    return 0;
  n = snprintf ((char *) (intptr_t) buf, (size_t) size, "%s", pending_messages);
  pending_messages = NULL;
  *seq += 2;
  return n;
}

typedef int entry_fn (int, int, int);
static entry_fn *const fns[RDPS_MOD_ENTRIES] = {
  m_attach, m_detach, m_set_file, m_start, m_stop, m_disconnect, m_reload, m_status, m_messages, m_set_mute, m_set_tls,
  m_set_sound, m_set_verbose };

/* modcall.s, here in C */
int
rdps_modcall (const void *fn, void *ws, int a, int b, int c)
{
  int i;
  last_ws = ws;
  for (i = 0; i < RDPS_MOD_ENTRIES; i++)
    if ((const void *) fns[i] == fn)
      calls[i]++;
  return ((entry_fn *) (intptr_t) fn) (a, b, c);
}

static const _kernel_oserror not_found = { 0x102, "Module not found" };

static const _kernel_oserror *
os_module (int swi, int r[10])
{
  (void) swi;
  if (r[0] == 18)
    {
      if (!loaded || strcmp ((const char *) (intptr_t) r[1], "RDPServer") != 0)
        return &not_found;
      r[3] = (int) (intptr_t) &module;
      r[4] = (int) (intptr_t) &ws_word;
      return NULL;
    }
  if (r[0] == 4)
    {
      killed++;
      snprintf (killed_name, sizeof killed_name, "%s", (const char *) (intptr_t) r[1]);
      return NULL;
    }
  abort ();
}

static const _kernel_oserror *
readsysinfo (int swi, int r[10])
{
  (void) swi;
  if (r[0] != 5)
    abort ();
  r[0] = 0x4C0;
  return NULL;
}

static char said[8][200];
static int nsaid;
static void hook (const char *line)
{
  if (nsaid < 8)
    snprintf (said[nsaid], sizeof said[0], "%s", line);
  nsaid++;
}

static int
said_has (const char *s)
{
  int i;
  for (i = 0; i < nsaid && i < 8; i++)
    if (strstr (said[i], s))
      return 1;
  return 0;
}

static void
write_file (const char *path, const char *text)
{
  FILE *f = fopen (path, "w");
  fputs (text, f);
  fclose (f);
}

int
main (void)
{
  char ini[128], keys[128], logins[128], xlog[128], line[200];
  server_options o;
  server_status st;
  FILE *f;
  int i;

  fake_reset ();
  fake_swi (OS_Module, "OS_Module", os_module);
  fake_swi (Wimp_ReadSysInfo, "Wimp_ReadSysInfo", readsysinfo);
  snprintf (dir, sizeof dir, "/tmp/rdps-modclient-%d", (int) getpid ());
  mkdir (dir, 0700);
  snprintf (ini, sizeof ini, "%s/xrdp.ini", dir);
  snprintf (keys, sizeof keys, "%s/rsakeys.ini", dir);
  snprintf (logins, sizeof logins, "%s/logins", dir);
  snprintf (xlog, sizeof xlog, "%s/XrdpLog", dir);
  write_file (ini, "[globals]\nsecurity_layer=rdp\n");
  write_file (keys, "[keys]\npub_exp=0x01\n");
  remove (logins);

  module.table.magic = RDPS_MOD_MAGIC;
  module.table.version = RDPS_MOD_INTERFACE;
  module.table.entries = RDPS_MOD_ENTRIES;
  for (i = 0; i < RDPS_MOD_ENTRIES; i++)
    module.table.offset[i] = (unsigned) ((intptr_t) fns[i] - (intptr_t) &module);

  memset (&o, 0, sizeof o);
  o.port = "3390";
  o.address = "";
  o.ini = ini;
  o.logins = logins;
  o.interval_cs = 10;
  o.mute_local = 1;
  o.require_tls = 1;
  o.send_sound = 1;
  o.rows_per_scan = 128;
  o.verbose = 1;
  o.log_file = xlog;
  server_set_message_hook (hook);

  /* No module */
  loaded = 0;
  CHECK (server_start (&o) == 1, "no module: server_start fails");
  CHECK (server_start_error () && strstr (server_start_error (), "isn't loaded"),
         "and says the module isn't loaded");
  CHECK (said_has ("isn't loaded"), "in the messages too");
  CHECK (server_step (0) == 25, "server_step without a module: look again in 25 cs");
  server_get_status (&st);
  CHECK (st.listening == 0, "and the status is empty");

  /* A module with another interface */
  loaded = 1;
  module.table.version = 1;
  module.table.entries = 9;
  CHECK (server_start (&o) == 1 && strstr (server_start_error (), "has interface 1,9; this !RDPServer needs 7,13"),
         "an older module (test10's): both versions said");
  module.table.entries = RDPS_MOD_ENTRIES;
  module.table.version = 99;
  CHECK (server_start (&o) == 1 && strstr (server_start_error (), "different version"),
         "a module of another version is refused");
  module.table.version = RDPS_MOD_INTERFACE;

  /* Another front end has it */
  attach_answer = 1;
  CHECK (server_start (&o) == 1 && strstr (server_start_error (), "Another"),
         "the module in use by another front end");
  attach_answer = 0;

  /* Normal start */
  nfiles = 0;
  CHECK (server_start (&o) == 0, "started");
  CHECK (server_start_error () == NULL, "no error");
  CHECK (attached_task == 0x4C0, "attached with our task handle");
  CHECK (last_ws == &ws_word, "every call has R12 = the module's workspace");
  CHECK (nfiles == 5 && strcmp (files[0], ini) == 0 && strstr (file_data[0], "security_layer"),
         "xrdp.ini given to the module under its name");
  CHECK (strcmp (files[1], keys) == 0 && strstr (file_data[1], "pub_exp"),
         "rsakeys.ini given");
  CHECK (strcmp (files[2], logins) == 0 && file_data[2][0] == 0,
         "no logins file: an empty one given");
  CHECK (strstr (files[3], "/key.pem") && file_null[3] && strstr (files[4], "/cert.pem")
         && file_null[4], "no TLS certificate: key.pem and cert.pem forgotten (no TLS)");
  CHECK (strcmp (started.port, "3390") == 0 && strcmp (started.ini, ini) == 0
         && strcmp (started.logins, logins) == 0 && started.interval_cs == 10
         && started.rows_per_scan == 128 && started.verbose == 1 && started.mute_local == 1
         && started.require_tls == 1 && started.send_sound == 1,
         "the options passed");

  /* Messages: the server's to the hook, libxrdp's to XrdpLog */
  nsaid = 0;
  pending_messages = "s client up: 1920 x 1080\nx [INFO ] xrdp_sec_incoming\ns second\n";
  connected = 1;
  CHECK (server_step (0) == 10, "while connected: look again in 10 cs");
  CHECK (nsaid == 2 && strcmp (said[0], "client up: 1920 x 1080") == 0
         && strcmp (said[1], "second") == 0, "the server's lines reach the hook");
  f = fopen (xlog, "r");
  CHECK (f && fgets (line, sizeof line, f) && strcmp (line, "[INFO ] xrdp_sec_incoming\n") == 0,
         "libxrdp's line reaches XrdpLog");
  if (f)
    fclose (f);
  server_get_status (&st);
  CHECK (st.listening == 1 && st.connected == 1 && st.width == 1920
         && strcmp (st.user, "andrew") == 0, "the status comes from the module");

  /* Disconnect, logins, stop */
  server_disconnect ();
  CHECK (calls[RDPS_MOD_DISCONNECT] == 1 && connected == 0, "Disconnect goes to the module");
  write_file (logins, "test 00 00\n");
  nfiles = 0;
  CHECK (server_reload_logins () == 0 && nfiles == 1 && strcmp (files[0], logins) == 0
         && strcmp (file_data[0], "test 00 00\n") == 0 && calls[RDPS_MOD_RELOAD_LOGINS] == 1,
         "new logins: the file again, then reload");
  server_set_mute (0);
  CHECK (calls[RDPS_MOD_SET_MUTE] == 1 && mute_arg == 0, "Choices' mute goes to the module");
  server_set_require_tls (0);
  CHECK (calls[RDPS_MOD_SET_REQUIRE_TLS] == 1 && tls_arg == 0, "and its TLS choice");
  server_set_sound (0);
  CHECK (calls[RDPS_MOD_SET_SOUND] == 1 && sound_arg == 0, "and its sound choice");
  server_set_verbose (1);
  CHECK (calls[RDPS_MOD_SET_VERBOSE] == 1 && verbose_arg == 1, "and its detailed log choice");
  server_finish ();
  CHECK (calls[RDPS_MOD_STOP] == 1, "server_finish stops it");
  CHECK (calls[RDPS_MOD_DETACH] == 0 && killed == 0, "but keeps the module");

  /* With TLS's key and certificate: both given */
  {
    char k[128], c[128];
    snprintf (k, sizeof k, "%s/key.pem", dir);
    snprintf (c, sizeof c, "%s/cert.pem", dir);
    write_file (k, "-----BEGIN RSA PRIVATE KEY-----\n");
    write_file (c, "-----BEGIN CERTIFICATE-----\n");
    nfiles = 0;
    CHECK (server_start (&o) == 0 && nfiles == 5 && strcmp (files[3], k) == 0 && !file_null[3]
           && strstr (file_data[3], "PRIVATE KEY") && strcmp (files[4], c) == 0
           && !file_null[4] && strstr (file_data[4], "CERTIFICATE"),
           "key.pem and cert.pem given to the module under their names");
    server_finish ();
    remove (c);
    write_file (k, "x");
    nfiles = 0;
    CHECK (server_start (&o) == 0 && file_null[3] && file_null[4],
           "only one of them: neither given (no half-made TLS)");
    server_finish ();
    remove (k);
  }

  /* A start the module refuses: its reason is in the messages */
  start_answer = 1;
  nsaid = 0;
  CHECK (server_start (&o) == 1, "the module's own failure");
  CHECK (said_has ("can't listen on port 80"), "its reason reaches the hook");
  CHECK (calls[RDPS_MOD_ATTACH] == 2, "attached once (and once refused), not again");
  start_answer = 0;

  /* Quitting */
  {
    int stops = calls[RDPS_MOD_STOP];
    server_shutdown ();
    CHECK (calls[RDPS_MOD_STOP] == stops + 1 && calls[RDPS_MOD_DETACH] == 1 && detach_arg == 0,
           "quit: stop and detach (not keeping it running)");
  }
  CHECK (killed == 1 && strcmp (killed_name, "RDPServer") == 0, "and the module is removed");
  CHECK (server_step (0) == 25, "nothing more to do after that");

  /* The desktop shutting down (0.2-test23): detach, keeping it running */
  CHECK (server_start (&o) == 0, "started again");
  {
    int stops = calls[RDPS_MOD_STOP], kills = killed, detaches = calls[RDPS_MOD_DETACH];
    server_leave_running ();
    CHECK (calls[RDPS_MOD_DETACH] == detaches + 1 && detach_arg == 1
           && calls[RDPS_MOD_STOP] == stops && killed == kills,
           "a shutdown: detach with keep, no stop, the module left loaded");
  }
  CHECK (server_step (0) == 25, "and nothing more to do");

  remove (ini);
  remove (keys);
  remove (logins);
  remove (xlog);
  rmdir (dir);
  return fake_done ("RDPServer front end's module calls (fake RISC OS)");
}
