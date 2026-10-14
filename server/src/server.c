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
#include "sound.h"
#include "input.h"
#include "auth.h"
#include "server.h"
#include "srvthread.h"

volatile int server_stop;
extern unsigned rdps_modexp_cs;   /* ssl_calls.c */
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
  unsigned resize_deadline;        /* cs; the client must be back by then */
  int palette_due;                 /* 8 bpp client reactivated: palette again */
  unsigned handshake_deadline;      /* cs; 0 once the client is up */
  unsigned tiles_sent;
  unsigned next_scan;              /* when the next band of rows is due */
  unsigned last_activity;          /* last change seen or input received */
  unsigned steps, reads, most_reads; /* for -v's figures every 5 s */
  unsigned step_cs_max, scans, stats_at;
  unsigned last_step_end, gap_max;  /* time between steps: other programs */
  char client_desc[64];            /* "192.168.0.58:58303" */
  char client_user[64];
  rdps_sound snd;                  /* sound to the client (sound.c) */
  int mute_local;                  /* from the options; Choices can change it */
  int running;
} server;

/* A client gets this long to finish connecting. libxrdp's waits during
   the connection sequence yield (srvthread.h), so other programs run
   meanwhile; this only stops a client that never finishes. */
#define HANDSHAKE_CS 3000
/* After this long without a change on the screen or input from the
   client, scan at the idle rate. */
#define ACTIVE_CS 200
/* -v logs any wait this long between steps while a client is connected. */
#define GAP_LOG_CS 25

static void server_arc_connection_ended (void);
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
    case 0x5559:   /* output suppressed (param1 1) or allowed again (0) */
      if (param1 == 0 && s->cap.frame)
        {
          cap_mark_all (&s->cap);
          s->palette_due = s->client_bpp == 8;
        }
      break;
    case 0x5555:   /* channel data: MAKELONG (channel id, flags), size, data, total */
      sound_channel_data (&s->snd, (int) (param1 & 0xffff), (int) ((param1 >> 16) & 0xffff),
                          (const void *) param3, (int) param2, (int) param4);
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
  sound_stop (&s->snd, s->session != NULL);
  if (s->session)
    {
      libxrdp_disconnect (s->session);
      libxrdp_exit (s->session);
      s->session = NULL;
    }
  if (s->trans)
    {
      struct trans *t = s->trans;
      s->trans = NULL;            /* (before it's freed: see module.c's lost) */
      trans_delete (t);
    }
  release_all (s);
  cap_free (&s->cap);
  free (s->tilebuf);
  s->tilebuf = NULL;
  s->was_up = 0;
  s->handshake_deadline = 0;
  server_arc_connection_ended ();
  plat_set_clients (0);
  s->client_desc[0] = s->client_user[0] = 0;
  say ("client gone (%s)", why);
}

/* --- auto-reconnect ([MS-RDPBCGR] 5.5) ---------------------------------------
   Once a client is up it gets a cookie: a logon id and 16 random bytes
   (TS_SAVE_SESSION_INFO_PDU_DATA, INFOTYPE_LOGON_EXTENDED_INF). If its
   connection then breaks off without a disconnect (the module's safety
   net cuts it when the server thread was lost), the client may connect
   again by itself, sending ARC_CS_PRIVATE_PACKET: the logon id and
   HMAC-MD5(key: the random bytes, data: the new connection's client
   random). That stands in for the password, for the same user. The
   cookie is kept outside the server's state (server_abandon clears
   that), and replaced for each client; the server stopping clears it. */
static struct
{
  int valid;
  int logon_id;
  unsigned char random[16];
  char user[64];
  unsigned ended_cs;      /* when its client's connection ended; 0 while up */
} arc;

/* A cookie is only good within this long of its connection ending. */
#define ARC_GRACE_CS (2 * 60 * 100)

/* The client's connection ended (or the server was lost): the cookie's
   grace time starts. */
static void
server_arc_connection_ended (void)
{
  if (arc.valid && arc.ended_cs == 0)
    arc.ended_cs = plat_time_cs () | 1;
}
static int arc_next_id = 1;

extern int rdps_hmac_md5 (const void *key, int key_len, const void *data, int data_len,
                          unsigned char out[16]);

/* 1 if the client presented this server's current cookie */
static int
arc_check (const struct xrdp_client_info *ci)
{
  unsigned char want[16], diff = 0;
  int i;
  if (!arc.valid || !ci->arc_present || ci->arc_logon_id != arc.logon_id
      || strcmp (ci->username, arc.user) != 0)
    return 0;
  if (arc.ended_cs == 0 || (int) (plat_time_cs () - arc.ended_cs) > ARC_GRACE_CS)
    return 0;           /* only to come back soon after a broken connection */
  if (rdps_hmac_md5 (arc.random, 16, ci->arc_client_random, 32, want) != 0)
    return 0;
  for (i = 0; i < 16; i++)              /* in constant time */
    diff |= (unsigned char) (want[i] ^ (unsigned char) ci->arc_verifier[i]);
  return diff == 0;
}

static void
arc_send (server *s, const struct xrdp_client_info *ci)
{
  /* infoType (4) = INFOTYPE_LOGON_EXTENDED_INF (3); TS_LOGON_INFO_EXTENDED:
     Length (2) = 38, FieldsPresent (4) = LOGON_EX_AUTORECONNECTCOOKIE,
     cbFieldData (4) = 28, ARC_SC_PRIVATE_PACKET (cbLen 28, Version 1,
     LogonId, ArcRandomBits[16]); Pad (570). */
  unsigned char d[4 + 38 + 570];
  unsigned char *p = d;
  memset (d, 0, sizeof d);
  arc.logon_id = arc_next_id++;
  g_random ((char *) arc.random, 16);
  copy_text (arc.user, sizeof arc.user, ci->username);
  arc.ended_cs = 0;
  arc.valid = 1;
#define PUT16(v) do { p[0] = (unsigned char) (v); p[1] = (unsigned char) ((v) >> 8); p += 2; } while (0)
#define PUT32(v) do { PUT16 ((v) & 0xffff); PUT16 (((unsigned) (v)) >> 16); } while (0)
  PUT32 (3);
  PUT16 (38);
  PUT32 (1);
  PUT32 (28);
  PUT32 (28);
  PUT32 (1);
  PUT32 (arc.logon_id);
  memcpy (p, arc.random, 16);
#undef PUT16
#undef PUT32
  if (libxrdp_send_session_info (s->session, (const char *) d, (int) sizeof d) != 0)
    arc.valid = 0;
}

/* Once the client is up: the depth it chose, the first frame. */
/* The screen's mode, read where output isn't going to a sprite: another
   program's turn may have it redirected (see plat_screen), so a read
   that says so is tried again (the thread yields meanwhile, so that
   program can finish), up to about half a second. NULL, or why not. */
static const char *
screen_read (rdps_screen *out)
{
  int tries;
  for (tries = 0; tries < 50; tries++)
    {
      const char *e = plat_screen (out);
      if (e)
        return e;
      if (!out->redirected)
        return NULL;
      rdps_wait_cs (1);
    }
  return "another program kept output switched to a sprite";
}

static int
session_up (server *s)
{
  struct xrdp_client_info *ci = s->session->client_info;

  /* The login, before the client is shown anything: the password, or
     this server's auto-reconnect cookie. */
  {
    int reconnect = arc_check (ci);
    if (reconnect)
      say ("auto-reconnect: \"%s\" is back (its cookie checked)", ci->username);
    if (s->logins.count > 0 && !reconnect
        && !auth_check (&s->logins, ci->username, ci->password))
      {
        say ("login refused for \"%s\"", ci->username);
        memset (ci->password, 0, sizeof ci->password);
        return 1;
      }
  }
  memset (ci->password, 0, sizeof ci->password);
  {
    /* The screen again: other programs ran while the client was
       connecting (a mode change meanwhile would leave the old one). */
    rdps_screen cur;
    if (screen_read (&cur) != NULL || cur.width != s->scr.width
        || cur.height != s->scr.height || cur.stride != s->scr.stride
        || pix_format_of (cur.log2bpp, cur.ncolour, cur.modeflags) != s->fmt)
      {
        say ("the screen mode changed while the client was connecting");
        return -1;
      }
    s->scr = cur;
  }
  if (s->opt->verbose)
    {
      say ("key exchange: the RSA step took %u cs", rdps_modexp_cs);
      if (rdps_sndbuf)
        say ("send buffer: %d KB", rdps_sndbuf / 1024);
      else
        say ("send buffer: the default (a bigger one was refused)");
    }
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
  arc_send (s, ci);
  sound_start (&s->snd, s->session, s->mute_local, s->opt->verbose, say);
  s->last_activity = plat_time_cs ();
  say ("client up: %d x %d, %d bpp, user \"%s\"", s->scr.width, s->scr.height,
       s->client_bpp, ci->username);
  plat_set_clients (1);
  return 0;
}

/* Sends the changed tiles. A whole screen takes a while to compress (the
   first frame, or everything changing at once), so every 2 cs the
   thread lets other programs run (srvthread.h): on RISC OS the desktop
   never stops for more than about that. */
#define SEND_SLICE_CS 2

static void
send_dirty (server *s)
{
  int i = 0, x, y, w, h;
  unsigned slice = plat_time_cs ();
  if (s->palette_due)
    {
      int pal[256];
      pix_palette_332 (pal);
      libxrdp_send_palette (s->session, pal);
      s->palette_due = 0;
    }
  while (cap_next_dirty (&s->cap, &i, &x, &y, &w, &h))
    {
      if ((int) (plat_time_cs () - slice) >= SEND_SLICE_CS)
        {
          rdps_wait_cs (0);
          slice = plat_time_cs ();
          if (!s->session || !s->trans || rdps_thread_ending ())
            return;
        }
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
  if (server_stop || rdps_thread_ending ())
    return 1;
  return s->handshake_deadline != 0
         && (int) (plat_time_cs () - s->handshake_deadline) >= 0;
}

static void
session_start (server *s, struct trans *t)
{
  const char *e = screen_read (&s->scr);

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
      session_end (s, rdps_thread_ending () ? "disconnected from this computer"
                      : is_term () && !server_stop ? "too slow connecting"
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
  s->snd.chan = -1;
  s->opt = o;
  s->mute_local = o->mute_local;
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

/* The screen mode changed while a client is connected. If the client said
   it can be resized ([MS-RDPBCGR] 2.2.7.1.2, desktopResizeFlag), its
   desktop is given the new size with a deactivation-reactivation
   sequence (libxrdp_reset, as xrdp does when a client resizes): the
   session carries on and the whole new screen is sent once the client is
   active again. Otherwise, or if the new mode can't be shared, the
   session ends. Returns 0 if it carries on. */
#define RESIZE_SETTLE_CS 20
#define RESIZE_TRIES 10

static int
same_mode (const rdps_screen *a, const rdps_screen *b)
{
  return a->width == b->width && a->height == b->height
         && a->log2bpp == b->log2bpp && a->stride == b->stride
         && a->ncolour == b->ncolour && a->modeflags == b->modeflags;
}

static int
screen_resized (server *s)
{
  struct xrdp_client_info *ci = s->session->client_info;
  rdps_screen cur, again;
  pix_format fmt;
  int tries;

  /* The desktop redraws everything after a mode change (and a program
     may change mode more than once): wait until it stays put. */
  if (screen_read (&cur) != NULL)
    {
      session_end (s, "the screen mode changed");
      return -1;
    }
  for (tries = 0; tries < RESIZE_TRIES; tries++)
    {
      rdps_wait_cs (RESIZE_SETTLE_CS);
      if (!s->session || !s->trans || rdps_thread_ending ())
        return -1;
      if (screen_read (&again) != NULL)
        {
          session_end (s, "the screen mode changed");
          return -1;
        }
      if (same_mode (&cur, &again))
        break;
      cur = again;
    }
  fmt = pix_format_of (cur.log2bpp, cur.ncolour, cur.modeflags);
  if (fmt == PIX_UNSUPPORTED)
    {
      session_end (s, "the screen mode changed to one that can't be shared "
                   "(use 16 or 32 bpp: thousands or millions of colours)");
      return -1;
    }
  if (cur.width == s->scr.width && cur.height == s->scr.height)
    {
      /* Same size (other colours, or another line length): the client's
         desktop stays as it is, and only the picture is sent again.
         Resizing it would have the client tear its window down and build
         it again (deactivation-reactivation), a flash the RISC OS screen
         never shows. */
      cap_free (&s->cap);
      s->scr = cur;
      s->fmt = fmt;
      if (cap_init (&s->cap, cur.width, cur.height, pix_bytes (fmt)) != 0)
        {
          session_end (s, "out of memory for the new screen mode");
          return -1;
        }
      cap_scan (&s->cap, s->scr.base, s->scr.stride, s->scr.height);
      cap_mark_all (&s->cap);
      s->last_activity = plat_time_cs ();
      say ("the screen mode changed, at the same size: the client's desktop "
           "stays as it is");
      return 0;
    }
  if (ci->client_resize_mode == CRMODE_NONE)
    {
      session_end (s, "the screen mode changed, and this client can't be "
                   "resized: connect again");
      return -1;
    }
  cap_free (&s->cap);
  s->scr = cur;
  s->fmt = fmt;
  g_rdps_desktop_width = cur.width;
  g_rdps_desktop_height = cur.height;
  ci->display_sizes.session_width = cur.width;
  ci->display_sizes.session_height = cur.height;
  ci->display_sizes.monitorCount = 0;   /* one screen, the new size */
  if (cap_init (&s->cap, cur.width, cur.height, pix_bytes (fmt)) != 0)
    {
      session_end (s, "out of memory for the new screen size");
      return -1;
    }
  if (libxrdp_reset (s->session) != 0)
    {
      session_end (s, "the screen mode changed, and resizing the client failed");
      return -1;
    }
  /* Nothing is sent while the client is reactivating (libxrdp sets the
     suppress-output mask until the client's font list comes); then
     everything (callback 0x5559). */
  s->resize_deadline = plat_time_cs () + HANDSHAKE_CS;
  cap_scan (&s->cap, s->scr.base, s->scr.stride, s->scr.height);
  cap_mark_all (&s->cap);
  s->last_activity = plat_time_cs ();
  say ("the screen mode changed: the client's desktop is now %d x %d", cur.width,
       cur.height);
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
          session_end (s, r > 0 ? "wrong user name or password"
                          : "couldn't start the session");
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
  /* Sound first: it goes on while the client is minimised (Suppress
     Output), not while it's reactivating after a resize. */
  if (s->was_up && s->session && s->trans && s->trans->wait_s == NULL
      && !s->resize_deadline)
    sound_step (&s->snd);
  /* While the client hasn't taken what we sent, don't send more: the
     changes wait as dirty tiles, so a slow link gets fewer, later frames
     rather than an ever-growing queue. */
  /* While the client is reactivating after a resize, or has asked for no
     updates (minimised: Suppress Output), send nothing. */
  if (s->was_up && s->session->client_info->suppress_output_mask)
    {
      if (s->resize_deadline && (int) (now - s->resize_deadline) >= 0)
        {
          session_end (s, "the client didn't come back after the resize");
          return 0;
        }
      return 2;
    }
  s->resize_deadline = 0;
  if (s->was_up && s->trans->wait_s == NULL && (int) (now - s->next_scan) >= 0)
    {
      rdps_screen cur;
      unsigned period = scan_period (s, now);
      int band, h = s->scr.height;
      /* A mode change moves or resizes the screen: stop reading the old
         one, and give the client the new size (or end the session). */
      const char *e = plat_screen (&cur);
      if (e == NULL && cur.redirected)
        {
          /* Output is going to a sprite in some program's turn (one
             that's running away, say, so this is where the module's
             time comes): the mode read is the sprite's, not the
             screen's. DisplayStart is still the screen's: follow its
             bank, keep the mode. (0.2-test5 took the sprite's mode for a
             mode change, and read the screen as if it had that mode.) */
          const unsigned char *shown = cur.base;
          size_t frame = s->scr.stride * (size_t) s->scr.height;
          uintptr_t d = (uintptr_t) shown - (uintptr_t) s->scr.base;
          cur = s->scr;
          /* Another bank of this mode (double-buffering): follow it. Any
             other move (a mode change meanwhile, the memory moved):
             don't read anything until a read that isn't redirected says
             what the screen is now. */
          if (shown == s->scr.base)
            ;
          else if (frame && s->scr.total
                   && (uintptr_t) (d + s->scr.total) < 2 * (uintptr_t) s->scr.total
                   && d % frame == 0)
            cur.base = shown;
          else
            {
              s->next_scan = now + 2;
              return 2;
            }
        }
      else if (e != NULL
          || cur.width != s->scr.width || cur.height != s->scr.height
          || cur.log2bpp != s->scr.log2bpp || cur.stride != s->scr.stride
          || pix_format_of (cur.log2bpp, cur.ncolour, cur.modeflags) != s->fmt)
        {
          screen_resized (s);
          return 0;
        }
      /* Only the displayed bank moved (a program double-buffering swaps
         banks with OS_Byte 113; 0.2-test2 took that for a mode change and
         ended the session): read the bank that's shown now. */
      s->scr.base = cur.base;
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
  unsigned t0 = plat_time_cs (), t, y0 = rdps_yielded_cs;
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
  /* the time it held the machine: not the waits in it (srvthread.h) */
  if (t - t0 - (rdps_yielded_cs - y0) > s->step_cs_max)
    s->step_cs_max = t - t0 - (rdps_yielded_cs - y0);
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

int
server_client_socket (void)
{
  return srv.trans ? (int) srv.trans->sck : -1;
}

void
server_disconnect (void)
{
  /* Dropped on purpose: no coming back on the auto-reconnect cookie, and
     libxrdp's goodbye (Disconnect Provider Ultimatum) tells the client so;
     without it the client would take it for a broken connection and
     connect again by itself. */
  memset (&arc, 0, sizeof arc);
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
  memset (&arc, 0, sizeof arc);     /* logins changed: password again */
  return 0;
}

/* Choices changed "mute this computer while a client listens": now, if
   sound is going to a client. */
void
server_set_mute (int on)
{
  server *s = &srv;
  s->mute_local = on != 0;
  s->snd.mute_local = s->mute_local;
  if (s->snd.stage == 3)
    plat_sound_mute (s->mute_local);
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
  memset (&arc, 0, sizeof arc);     /* no reconnecting to a stopped server */
  say ("stopped (%u tiles sent)", s->tiles_sent);
  log_end ();
}

/* The server thread was lost in the middle of something (srvthread.c's
   rdps_thread_reset): let go of anything held on RISC OS and forget the
   rest. The screen copy and the tile buffer (the big ones: 8 MB at
   1920 x 1080) are freed, as nothing changes them in the middle of a
   wait; libxrdp's session and the logins are left, since they may be
   half changed (a few KB each time). */
void
server_abandon (void)
{
  server *s = &srv;
  release_all (s);
  sound_stop (&s->snd, 0);         /* the handler out, this computer unmuted */
  server_arc_connection_ended ();
  cap_free (&s->cap);
  free (s->tilebuf);
  plat_set_clients (0);
  if (s->running)
    log_end ();                 /* server_start starts libxrdp's log again */
  memset (s, 0, sizeof *s);
}

void
server_shutdown (void)
{
  server_finish ();
}

const char *
server_start_error (void)
{
  return NULL;
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
