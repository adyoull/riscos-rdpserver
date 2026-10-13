/* RDPServer: the server loop, the libxrdp session and its input.

   The way a session is driven (the trans set-up, reading whole PDUs in
   xrdp_session_data_in, libxrdp_process_incoming then
   libxrdp_process_data) follows xrdp's xrdp/xrdp_process.c (xrdp 0.10.6,
   Apache-2.0, Copyright (C) Jay Sorg 2004-2014), which does the same for
   xrdp's own window manager.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include "arch.h"
#include "os_calls.h"
#include "parse.h"
#include "trans.h"
#include "libxrdpinc.h"
#include "ms-rdpbcgr.h"
#include "xrdp_client_info.h"
#include "log.h"

#include "platform.h"
#include "pixels.h"
#include "capture.h"
#include "keymap.h"
#include "input.h"
#include "auth.h"
#include "server.h"

volatile int server_stop;
int g_rdps_desktop_width, g_rdps_desktop_height;

typedef struct
{
  const server_options *opt;
  struct trans *listener;
  struct trans *trans;             /* the client's connection, or NULL */
  struct xrdp_session *session;
  int was_up;                      /* session->up_and_running last time */
  rdps_screen scr;
  pix_format fmt;
  capture cap;
  int client_bpp;
  unsigned char *tilebuf;          /* one tile in the client's depth */
  inq input;                       /* input waiting to go to RISC OS */
  inq_sink sink;
  auth_db logins;
  unsigned refuse_until;           /* after a wrong password: cs */
  unsigned handshake_deadline;      /* cs; 0 once the client is up */
  unsigned tiles_sent;
  unsigned next_scan;              /* when the next band of rows is due */
  unsigned last_activity;          /* last change seen or input received */
  unsigned steps, reads, most_reads; /* for -v's figures every 5 s */
  unsigned step_cs_max, scans, stats_at;
  unsigned last_step_end, gap_max;  /* time between steps: other programs */
  char client_desc[64];            /* "192.168.0.58:58303" */
  char client_user[64];
  int running;
} server;

/* A client gets this long to finish connecting: libxrdp reads the
   connection sequence with blocking reads, and nothing else runs
   meanwhile. */
#define HANDSHAKE_CS 3000
/* After this long without a change on the screen or input from the
   client, scan at the idle rate. */
#define ACTIVE_CS 200
/* -v logs any wait this long between steps while a client is connected. */
#define GAP_LOG_CS 25

static server srv;

/* Messages: to stdout (the task window, Linux), or to the front end's
   hook (the Wimp version keeps the last few for its Status window and
   writes them to a log file). */
static void (*say_hook) (const char *line);

void
server_set_message_hook (void (*hook) (const char *line))
{
  say_hook = hook;
}

static void
say (const char *fmt, ...)
{
  char line[256];
  va_list ap;
  va_start (ap, fmt);
  vsnprintf (line, sizeof line, fmt, ap);
  va_end (ap);
  if (say_hook)
    say_hook (line);
  else
    {
      printf ("RDPServer: %s\n", line);
      fflush (stdout);
    }
}

/* Copies text, cut short to fit (user names can be long). */
static void
copy_text (char *to, size_t size, const char *from)
{
  size_t n = strlen (from);
  if (n >= size)
    n = size - 1;
  memcpy (to, from, n);
  to[n] = 0;
}

/* --- input ---------------------------------------------------------------- */

static void
sink_move (void *ctx, int x, int y)
{
  server *s = ctx;
  plat_mouse_to (&s->scr, x, y);
}

static int loggable (int ll);

/* Before a mouse button goes down: if RISC OS thinks Shift, Ctrl or Alt
   is held but the client hasn't pressed one, let go of it, or the click
   is taken as a Shift-click (test2 on a Pi 4: every Select acted as
   Shift-Select with no Shift from the client). A key-up for a key that's
   already up does nothing, so this can't upset anything. */
static void
unstick_modifiers (server *s)
{
  static const struct { int bit, left, right; const char *name; } mod[] = {
    { PLAT_SHIFT, 0x4C, 0x58, "Shift" },
    { PLAT_CTRL, 0x3B, 0x61, "Ctrl" },
    { PLAT_ALT, 0x5E, 0x60, "Alt" } };
  int m = plat_modifiers ();
  size_t i;

  if (s->opt->verbose)
    say ("before the click: RISC OS has%s%s%s held, keyboard status &%02X",
         (m & PLAT_SHIFT) ? " Shift" : "", (m & PLAT_CTRL) ? " Ctrl" : "",
         (m & PLAT_ALT) ? " Alt" : (m & 7) ? "" : " nothing", (unsigned) (m >> 8) & 0xff);
  for (i = 0; i < sizeof mod / sizeof mod[0]; i++)
    if ((m & mod[i].bit) && !s->input.applied[mod[i].left]
        && !s->input.applied[mod[i].right])
      {
        say ("%s was held on RISC OS though the client hadn't pressed it: "
             "letting go of it", mod[i].name);
        plat_key (mod[i].left, 0);
        plat_key (mod[i].right, 0);
      }
}

static void
sink_key (void *ctx, int ll, int down)
{
  server *s = ctx;
  if (down && ll >= 0x70 && ll <= 0x72)
    unstick_modifiers (s);
  if (s->opt->verbose)
    {
      if (loggable (ll))
        say ("KeyV %s &%02X", down ? "down" : "up", ll);
      else
        say ("KeyV %s (a key)", down ? "down" : "up");
    }
  plat_key (ll, down);
}

static int
sink_click (void *ctx, int ll)
{
  server *s = ctx;
  unstick_modifiers (s);
  if (plat_click (ll, INQ_CLICK_CS) != 0)
    return -1;
  if (s->opt->verbose)
    say ("click &%02X: KeyV down, up in %d cs by ticker", ll, INQ_CLICK_CS);
  return 0;
}

static unsigned
sink_now (void *ctx)
{
  (void) ctx;
  return plat_time_cs ();
}

static void
sink_wheel (void *ctx, int dz, int dw)
{
  server *s = ctx;
  if (s->opt->verbose)
    say ("wheel %d, %d", dz, dw);
  plat_wheel (dz, dw);
}

/* Nothing must stay pressed on RISC OS when the client goes. */
static void
release_all (server *s)
{
  inq_release_all (&s->input, &s->sink);
}

/* If the program stops for any other reason, let go of the keys first. */
static void
at_exit (void)
{
  plat_click_finish ();
  release_all (&srv);
  plat_set_clients (0);
}

static void
fatal_signal (int sig)
{
  plat_click_finish ();
  release_all (&srv);
  signal (sig, SIG_DFL);
  raise (sig);
}

/* RDP's 9-bit wheel rotation: bit 8 is the sign. */
static int
wheel_rotation (int flags)
{
  int r = flags & 0x1ff;
  return (r & 0x100) ? r - 0x200 : r;
}

static void
mouse (server *s, int flags, int x, int y)
{
  int down = (flags & PTRFLAGS_DOWN) != 0;

  if (s->opt->verbose && (flags & ~PTRFLAGS_MOVE))
    say ("mouse flags &%04X at %d,%d", (unsigned) flags, x, y);
  if (flags & PTRFLAGS_WHEEL)
    {
      inq_wheel (&s->input, wheel_rotation (flags), 0);
      return;
    }
  if (flags & PTRFLAGS_HWHEEL)
    {
      inq_wheel (&s->input, wheel_rotation (flags), 1);
      return;
    }
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= s->scr.width) x = s->scr.width - 1;
  if (y >= s->scr.height) y = s->scr.height - 1;
  inq_move (&s->input, x, y);
  /* A PC mouse on RISC OS: left Select, middle Menu, right Adjust. */
  if (flags & PTRFLAGS_BUTTON1)
    inq_key (&s->input, 0x70, down);
  if (flags & PTRFLAGS_BUTTON3)
    inq_key (&s->input, 0x71, down);
  if (flags & PTRFLAGS_BUTTON2)
    inq_key (&s->input, 0x72, down);
}

/* Shift, Ctrl, Alt, Caps Lock, Num Lock and the mouse buttons: the keys
   -v names in the log. Other keys are logged without saying which, so a
   password typed over RDP never ends up in the log. */
static int
loggable (int ll)
{
  switch (ll)
    {
    case 0x4C: case 0x58: case 0x3B: case 0x61: case 0x5E: case 0x60:
    case 0x5D: case 0x22: case 0x70: case 0x71: case 0x72:
      return 1;
    }
  return 0;
}

static void
scancode (server *s, int flags, int code)
{
  int ll = keymap_lookup (code, (flags & KBD_FLAG_EXT) != 0);
  if (s->opt->verbose && loggable (ll))
    say ("key scancode &%02X flags &%04X -> &%02X", (unsigned) code,
         (unsigned) flags, (unsigned) ll);
  else if (s->opt->verbose && ll < 0)
    say ("key scancode &%02X flags &%04X: no RISC OS key", (unsigned) code,
         (unsigned) flags);
  if (ll >= 0)
    inq_key (&s->input, ll, (flags & KBD_FLAG_UP) == 0);
}

/* libxrdp's callback: input from the client, and its requests. */
static int
callback (intptr_t id, int msg, intptr_t param1, intptr_t param2,
          intptr_t param3, intptr_t param4)
{
  server *s = (server *) id;
  if (s == NULL || s->session == NULL)
    return 0;
  s->last_activity = plat_time_cs ();
  switch (msg)
    {
    case RDP_INPUT_SCANCODE:
      scancode (s, (int) param3, (int) param1);
      break;
    case RDP_INPUT_SYNCHRONIZE:
      /* Sent when the client window gets the focus back: any key-ups
         sent while it hadn't got it are lost, so let go of everything
         (the client sends key-downs for keys still held). */
      if (s->opt->verbose)
        say ("synchronise (lock keys &%X)", (unsigned) param1);
      release_all (s);
      break;
    case RDP_INPUT_MOUSE:
      mouse (s, (int) param3, (int) param1, (int) param2);
      break;
    case 0x4444:   /* the client asks for a rectangle to be redrawn */
      if (s->cap.frame)
        cap_mark (&s->cap, (int) param1, (int) param2, (int) param3, (int) param4);
      break;
    default:
      break;
    }
  return 0;
}

/* --- the session ---------------------------------------------------------- */

static void
session_end (server *s, const char *why)
{
  if (s->session)
    {
      libxrdp_disconnect (s->session);
      libxrdp_exit (s->session);
      s->session = NULL;
    }
  if (s->trans)
    {
      trans_delete (s->trans);
      s->trans = NULL;
    }
  release_all (s);
  cap_free (&s->cap);
  free (s->tilebuf);
  s->tilebuf = NULL;
  s->was_up = 0;
  s->handshake_deadline = 0;
  plat_set_clients (0);
  s->client_desc[0] = s->client_user[0] = 0;
  say ("client gone (%s)", why);
}

/* Once the client is up: the depth it chose, the first frame. */
static int
session_up (server *s)
{
  struct xrdp_client_info *ci = s->session->client_info;

  /* The login, before the client is shown anything. */
  if (s->logins.count > 0 && !auth_check (&s->logins, ci->username, ci->password))
    {
      say ("login refused for \"%s\"", ci->username);
      memset (ci->password, 0, sizeof ci->password);
      return 1;
    }
  memset (ci->password, 0, sizeof ci->password);
  s->client_bpp = ci->bpp;
  if (cap_init (&s->cap, s->scr.width, s->scr.height, pix_bytes (s->fmt)) != 0)
    return -1;
  s->tilebuf = malloc ((size_t) CAP_TILE * CAP_TILE * 4);
  if (!s->tilebuf)
    return -1;
  if (s->client_bpp == 8)
    {
      int pal[256];
      pix_palette_332 (pal);
      libxrdp_send_palette (s->session, pal);
    }
  cap_scan (&s->cap, s->scr.base, s->scr.stride, s->scr.height);
  cap_mark_all (&s->cap);
  copy_text (s->client_user, sizeof s->client_user, ci->username);
  s->last_activity = plat_time_cs ();
  say ("client up: %d x %d, %d bpp, user \"%s\"", s->scr.width, s->scr.height,
       s->client_bpp, ci->username);
  plat_set_clients (1);
  return 0;
}

static void
send_dirty (server *s)
{
  int i = 0, x, y, w, h;
  while (cap_next_dirty (&s->cap, &i, &x, &y, &w, &h))
    {
      pix_convert (s->fmt, cap_pixel (&s->cap, x, y), s->cap.row_bytes, w, h,
                   s->client_bpp, s->tilebuf);
      libxrdp_send_bitmap (s->session, w, h, s->client_bpp,
                           (char *) s->tilebuf, x, y, w, h);
      s->tiles_sent++;
    }
}

/* Reads whole PDUs and passes them to libxrdp (as xrdp_process_data_in). */
static int
session_data_in (struct trans *t)
{
  server *s = (server *) t->callback_data;
  struct stream *in = t->in_s;
  int len;

  switch (t->extra_flags)
    {
    case 0:
      if (libxrdp_process_data (s->session, 0) != 0)
        return 1;
      if (s->session->up_and_running)
        {
          t->header_size = 2;
          t->extra_flags = 1;
          init_stream (in, 0);
        }
      break;
    case 1:
      /* the first 2 bytes: TPKT (3) or fast-path */
      if (in->p[0] == 3)
        {
          t->header_size = 4;
          t->extra_flags = 2;
        }
      else if (in->p[1] & 0x80)
        {
          t->header_size = 3;
          t->extra_flags = 2;
        }
      else
        {
          t->header_size = (tui8) in->p[1];
          t->extra_flags = 3;
        }
      if (t->header_size > (unsigned) (in->end - in->data))
        break;
      /* FALLTHROUGH */
    case 2:
      /* enough to know the PDU's length */
      len = libxrdp_get_pdu_bytes (in->p);
      if (len == -1)
        return 1;
      t->header_size = (unsigned) len;
      t->extra_flags = 3;
      if (t->header_size > (unsigned) (in->end - in->data))
        break;
      /* FALLTHROUGH */
    case 3:
      /* the whole PDU */
      in->p = in->data;
      if (libxrdp_process_data (s->session, in) != 0)
        return 1;
      init_stream (in, 0);
      t->header_size = 2;
      t->extra_flags = 1;
      break;
    }
  return 0;
}

static int
is_term (void)
{
  server *s = &srv;
  if (server_stop)
    return 1;
  return s->handshake_deadline != 0
         && (int) (plat_time_cs () - s->handshake_deadline) >= 0;
}

static void
session_start (server *s, struct trans *t)
{
  const char *e = plat_screen (&s->scr);

  if (e)
    {
      say ("can't read the screen: %s", e);
      trans_delete (t);
      return;
    }
  s->fmt = pix_format_of (s->scr.log2bpp, s->scr.ncolour, s->scr.modeflags);
  if (s->fmt == PIX_UNSUPPORTED)
    {
      say ("this screen mode can't be shared: use 16 or 32 bpp (thousands "
           "or millions of colours)");
      trans_delete (t);
      return;
    }
  g_rdps_desktop_width = s->scr.width;
  g_rdps_desktop_height = s->scr.height;

  s->trans = t;
  s->handshake_deadline = plat_time_cs ()
                          + (s->opt->handshake_cs ? s->opt->handshake_cs : HANDSHAKE_CS);
  t->extra_flags = 0;
  t->header_size = 0;
  t->no_stream_init_on_data_in = 1;
  t->trans_data_in = session_data_in;
  t->callback_data = s;
  t->is_term = is_term;
  init_stream (t->in_s, 8192 * 4);
  s->session = libxrdp_init ((tbus) s, t, s->opt->ini);
  if (!s->session)
    {
      session_end (s, "libxrdp_init failed");
      return;
    }
  t->si = &s->session->si;
  t->my_source = XRDP_SOURCE_CLIENT;
  s->session->callback = callback;
  s->session->is_term = is_term;
  g_sck_get_peer_description ((int) t->sck, s->client_desc, sizeof s->client_desc);
  s->client_user[0] = 0;
  say ("connection from %s", s->client_desc);
  /* The connection sequence, up to the client's capabilities. */
  if (libxrdp_process_incoming (s->session) != 0)
    {
      session_end (s, is_term () && !server_stop ? "too slow connecting"
                                                 : "the connection sequence failed");
      return;
    }
  init_stream (t->in_s, 32 * 1024);
}

static int
conn_in (struct trans *listener, struct trans *t)
{
  server *s = (server *) listener->callback_data;
  if (s->refuse_until && (int) (plat_time_cs () - s->refuse_until) < 0)
    {
      trans_delete (t);
      return 0;
    }
  if (s->trans != NULL)
    {
      char desc[64];
      say ("refused %s: a client is already connected",
           g_sck_get_peer_description ((int) t->sck, desc, sizeof desc));
      trans_delete (t);
      return 0;
    }
  session_start (s, t);
  return 0;
}

/* --- the loop ------------------------------------------------------------- */

int
server_start (const server_options *o)
{
  server *s = &srv;

  memset (s, 0, sizeof *s);
  s->opt = o;
  inq_init (&s->input);
  s->sink.move = sink_move;
  s->sink.key = sink_key;
  s->sink.wheel = sink_wheel;
  s->sink.now = sink_now;
  s->sink.click = sink_click;
  s->sink.ctx = s;
  if (auth_load (o->logins, &s->logins) != 0)
    {
      say ("can't read the logins file (%s)", o->logins);
      return 1;
    }
  if (s->logins.count == 0)
    say ("no login set: anyone who can reach this computer can connect");
  plat_set_clients (0);
  {
    static int once;
    if (!once)
      {
        once = 1;
        atexit (at_exit);
        signal (SIGSEGV, fatal_signal);
        signal (SIGBUS, fatal_signal);
        signal (SIGILL, fatal_signal);
        signal (SIGFPE, fatal_signal);
        signal (SIGABRT, fatal_signal);
      }
  }
  {
    /* libxrdp's messages: errors only, or more with -v; to the console,
       or to a file (the Wimp version: a Wimp task mustn't print). */
    struct log_config *lc = log_config_init_for_console (o->verbose ? LOG_LEVEL_INFO
                                                         : LOG_LEVEL_ERROR, NULL);
    if (lc)
      {
        if (o->log_file)
          {
            lc->enable_console = 0;
            lc->log_file = strdup (o->log_file);
            lc->log_level = o->verbose ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR;
          }
        log_start_from_param (lc);
        log_config_free (lc);
      }
  }
  s->listener = trans_create (TRANS_MODE_TCP, 8192, 8192);
  if (!s->listener)
    {
      log_end ();
      return 1;
    }
  s->listener->trans_conn_in = conn_in;
  s->listener->callback_data = s;
  s->listener->is_term = is_term;
  if (trans_listen_address (s->listener, o->port, o->address) != 0)
    {
      say ("can't listen on port %s: %s", o->port, g_get_strerror ());
      trans_delete (s->listener);
      s->listener = NULL;
      log_end ();
      return 1;
    }
  s->running = 1;
  say ("listening on port %s", o->port);
  return 0;
}

/* The time between scans of the whole screen: quicker while things are
   happening, slower when the screen has been still for a while. */
static unsigned
scan_period (server *s, unsigned now)
{
  unsigned active = s->opt->interval_cs ? s->opt->interval_cs : 10;
  unsigned idle = s->opt->idle_interval_cs ? s->opt->idle_interval_cs : 25;
  return (int) (now - s->last_activity) < ACTIVE_CS ? active : idle;
}

static int
step (unsigned max_wait_cs)
{
  server *s = &srv;
  fd_set rd, wr;
  struct timeval tv;
  int backlog, maxfd;
  unsigned now = plat_time_cs (), wait_cs;

  if (!s->running)
    return 25;
  maxfd = (int) s->listener->sck;
  FD_ZERO (&rd);
  FD_ZERO (&wr);
  FD_SET ((int) s->listener->sck, &rd);
  /* Output libxrdp couldn't send yet waits in trans->wait_s. */
  backlog = s->trans && s->trans->wait_s != NULL;
  if (s->trans)
    {
      FD_SET ((int) s->trans->sck, &rd);
      if (backlog)
        FD_SET ((int) s->trans->sck, &wr);
      if ((int) s->trans->sck > maxfd)
        maxfd = (int) s->trans->sck;
    }
  if (s->was_up && !backlog)
    wait_cs = (int) (s->next_scan - now) > 0 ? s->next_scan - now : 0;
  else
    wait_cs = 20;
  if (s->input.count > 0 && wait_cs > 1)
    wait_cs = 1;   /* input waiting for its turn */
  if (wait_cs > max_wait_cs)
    wait_cs = max_wait_cs;
  tv.tv_sec = (long) (wait_cs / 100);
  tv.tv_usec = (long) (wait_cs % 100) * 10000;
  select (maxfd + 1, &rd, &wr, NULL, &tv);

  if (FD_ISSET ((int) s->listener->sck, &rd)
      && trans_check_wait_objs (s->listener) != 0)
    {
      /* An accept() error (a client that reset before we took it)
         marks the listener down for good: keep it listening. */
      s->listener->status = TRANS_STATUS_UP;
    }

  /* Reads what has come in, and sends what's waiting
     (trans_check_wait_objs ends with trans_send_waiting). One call reads
     only up to the next stage of a PDU (its first bytes, its length,
     then the rest), so keep going while there's more: the desktop
     version gets here only about every 2 cs, and a client sends a mouse
     move every 1-2 cs. Moves in a row are merged by the input queue, so
     only the last one reaches RISC OS. A time limit keeps the desktop
     moving while a client floods us. */
  if (s->trans && (FD_ISSET ((int) s->trans->sck, &rd)
                   || (backlog && FD_ISSET ((int) s->trans->sck, &wr))))
    {
      unsigned until = plat_time_cs () + 3;
      int n = 0;
      do
        {
          if (trans_check_wait_objs (s->trans) != 0)
            {
              session_end (s, "disconnected");
              return 0;
            }
          n++;
        }
      while (n < 1000 && (int) (plat_time_cs () - until) < 0
             && g_sck_can_recv ((int) s->trans->sck, 0));
      s->reads += (unsigned) n;
      if ((unsigned) n > s->most_reads)
        s->most_reads = (unsigned) n;
    }
  inq_run (&s->input, plat_time_cs (), &s->sink);
  if (s->input.clicked)
    {
      /* Let the Wimp see the button down now, before its up (by ticker,
         INQ_CLICK_CS from now): no scanning or sending this time. */
      s->input.clicked = 0;
      return 0;
    }

  if (s->trans && !s->was_up && s->handshake_deadline && is_term ())
    {
      session_end (s, "too slow connecting");
      return 0;
    }

  if (s->session && s->session->up_and_running && !s->was_up)
    {
      int r = session_up (s);
      if (r != 0)
        {
          if (r > 0)
            /* Slow down password guessing: no new clients for 3 s. */
            s->refuse_until = plat_time_cs () + 300;
          session_end (s, r > 0 ? "wrong user name or password" : "out of memory");
          return 0;
        }
      s->was_up = 1;
      s->handshake_deadline = 0;
      send_dirty (s);
      s->next_scan = plat_time_cs () + scan_period (s, plat_time_cs ());
    }
  else if (s->session && !s->session->up_and_running && s->was_up)
    {
      /* deactivated (a reset): wait to be up again */
      s->was_up = 0;
      cap_free (&s->cap);
      free (s->tilebuf);
      s->tilebuf = NULL;
    }

  now = plat_time_cs ();
  /* While the client hasn't taken what we sent, don't send more: the
     changes wait as dirty tiles, so a slow link gets fewer, later frames
     rather than an ever-growing queue. */
  if (s->was_up && s->trans->wait_s == NULL && (int) (now - s->next_scan) >= 0)
    {
      rdps_screen cur;
      unsigned period = scan_period (s, now);
      int band, h = s->scr.height;
      /* A mode change moves or resizes the screen: stop reading the old
         one. The client can connect again to get the new size. */
      if (plat_screen (&cur) != NULL || cur.base != s->scr.base
          || cur.width != s->scr.width || cur.height != s->scr.height
          || cur.log2bpp != s->scr.log2bpp || cur.stride != s->scr.stride
          || pix_format_of (cur.log2bpp, cur.ncolour, cur.modeflags) != s->fmt)
        {
          session_end (s, "the screen mode changed");
          return 0;
        }
      /* The screen is read in bands, so no one scan holds the machine up
         for long: a band every period * band / height. */
      band = s->opt->rows_per_scan > 0 ? s->opt->rows_per_scan : h;
      if (band > h)
        band = h;
      s->scans++;
      if (cap_scan (&s->cap, s->scr.base, s->scr.stride, band) > 0)
        s->last_activity = now;
      send_dirty (s);
      {
        unsigned gap = (unsigned) ((unsigned long) period * band / h);
        s->next_scan = now + (gap ? gap : 1);
      }
    }

  if (s->input.count > 0)
    return 1;
  if (s->trans && s->trans->wait_s != NULL)
    return 2;   /* the client is behind: no scan until it catches up */
  if (s->was_up)
    {
      int d = (int) (s->next_scan - plat_time_cs ());
      return d > 0 ? (d < 2 ? d : 2) : 0;   /* the socket every 2 cs at most */
    }
  return s->trans ? 2 : 25;
}

/* One step, timed; with -v, figures every 5 s while a client is there. */
int
server_step (unsigned max_wait_cs)
{
  server *s = &srv;
  unsigned t0 = plat_time_cs (), t;
  int r, held;

  /* How long other programs had the machine since the last step. With -v,
     a long wait while a client is connected is logged at once, saying
     whether a mouse button was held (a drag) at the time. */
  if (s->last_step_end && s->trans)
    {
      unsigned gap = t0 - s->last_step_end;
      if (gap > s->gap_max)
        s->gap_max = gap;
      held = s->input.applied[0x70] || s->input.applied[0x71] || s->input.applied[0x72];
      if (gap >= GAP_LOG_CS && s->opt && s->opt->verbose)
        say ("no turn for %u cs%s", gap, held ? " (a mouse button held)" : "");
    }
  r = step (max_wait_cs);

  t = plat_time_cs ();
  s->last_step_end = t;
  s->steps++;
  if (t - t0 > s->step_cs_max)
    s->step_cs_max = t - t0;
  if (s->stats_at == 0)
    s->stats_at = t + 500;
  else if ((int) (t - s->stats_at) >= 0)
    {
      if (s->opt && s->opt->verbose && s->trans)
        say ("last 5 s: %u steps, %u reads (most %u in one step), %u scans, "
             "longest step %u cs, longest wait %u cs, %u tiles sent in all",
             s->steps, s->reads, s->most_reads, s->scans, s->step_cs_max,
             s->gap_max, s->tiles_sent);
      s->steps = s->reads = s->most_reads = s->scans = s->step_cs_max = 0;
      s->gap_max = 0;
      s->stats_at = t + 500;
    }
  return r;
}

void
server_disconnect (void)
{
  if (srv.trans || srv.session)
    session_end (&srv, "disconnected from this computer");
}

int
server_reload_logins (void)
{
  server *s = &srv;
  auth_db db;
  if (!s->opt || auth_load (s->opt->logins, &db) != 0)
    return -1;
  s->logins = db;
  return 0;
}

void
server_finish (void)
{
  server *s = &srv;
  if (!s->running)
    return;
  if (s->session || s->trans)
    session_end (s, "server stopping");
  trans_delete (s->listener);
  s->listener = NULL;
  s->running = 0;
  say ("stopped (%u tiles sent)", s->tiles_sent);
  log_end ();
}

void
server_get_status (server_status *st)
{
  server *s = &srv;
  memset (st, 0, sizeof *st);
  st->listening = s->running;
  st->connected = s->was_up;
  st->connecting = s->trans != NULL && !s->was_up;
  copy_text (st->client, sizeof st->client, s->client_desc);
  copy_text (st->user, sizeof st->user, s->client_user);
  st->width = s->scr.width;
  st->height = s->scr.height;
  st->bpp = s->client_bpp;
  st->tiles_sent = s->tiles_sent;
  st->login_set = s->logins.count > 0;
}

int
server_run (const server_options *o)
{
  unsigned start = plat_time_cs ();
  if (server_start (o) != 0)
    return 1;
  while (!server_stop)
    {
      if (o->max_seconds && plat_time_cs () - start >= (unsigned) o->max_seconds * 100)
        break;
      if (o->step_gap_cs)
        {
          server_step (0);
          usleep (o->step_gap_cs * 10000);
        }
      else
        server_step (20);
    }
  server_finish ();
  return 0;
}
