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
#include <sys/socket.h>
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
#include "pointer.h"
#include "login.h"
#include "kbdlayout.h"
#include "capture.h"
#include "keymap.h"
#include "sound.h"
#include "input.h"
#include "auth.h"
#include "server.h"
#include "srvthread.h"
#include "tls.h"
#include "xrdp_peek.h"
#include "encspeed.h"

volatile int server_stop;
volatile int server_prereset;
volatile int server_shutdown_seen;
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
  uint32_t pal[256];               /* the palette (2 to 256 colour modes) */
  int pal_n;
  capture cap;
  int client_bpp;
  unsigned char *tilebuf;          /* one tile in the client's depth */
  inq input;                       /* input waiting to go to RISC OS */
  inq_sink sink;
  auth_db logins;
  unsigned refuse_until;           /* after a wrong password: cs */
  unsigned resize_deadline;        /* cs; the client must be back by then */
  int palette_due;                 /* 8 bpp client reactivated: palette again */
  int pointer_due;                 /* the pointer shape to send (pointer.h) */
  /* the login screen (login.h), while the client hasn't logged in */
  login_ui login;
  int in_login, login_due, login_tries;
  unsigned login_until, login_wait;
  int go_due;                      /* logged in during a resize: session_go
                                      once the client is back */
  unsigned up_since;               /* when the client came up (cs) */
  unsigned stall_bytes, stall_since; /* sent so far, and since when output
                                      has waited with nothing going */
  unsigned last_rtt_cs;            /* the last network check's round trip */
  unsigned handshake_deadline;      /* cs; 0 once the client is up */
  unsigned tiles_sent;
  unsigned next_scan;              /* when the next band of rows is due */
  unsigned last_activity;          /* last change seen or input received */
  unsigned steps, reads, most_reads; /* for -v's figures every 5 s */
  unsigned step_cs_max, scans, stats_at;
  unsigned stats_bytes;            /* rdps_bytes_sent when the 5 s began */
  int scan_rows;                   /* most rows a turn (0: no limit) */
  int sample;                      /* read 1 row in this many a pass (1: all) */
  unsigned scan_at;                /* when the last band was read */
  unsigned long long stats_read;   /* cap.bytes_read when the 5 s began */
  unsigned pass_start, pass_cs;    /* the current pass over the screen began;
                                      the last whole one took */
  unsigned last_step_end, gap_max;  /* time between steps: other programs */
  char client_desc[64];            /* "192.168.0.58:58303" */
  char client_user[64];
  rdps_sound snd;                  /* sound to the client (sound.c) */
  int mute_local;                  /* from the options; Choices can change it */
  int require_tls;                 /* likewise */
  int send_sound;                  /* likewise: off unless chosen (0.2-test22) */
  int sound_stop_due;              /* Choices turned it off: stop it in step */
  int sound_closed;                /* stopped by Choices: not again this session */
  int fills_var, bulk_var;         /* RDPServer$Fills, $BulkCompress (-1: unset) */
  int fills_ok;                    /* this client takes OpaqueRect orders */
  int unicode_said, unicode_bad_said; /* logged this session (0.2-test23) */
  int alphabet;                    /* RISC OS's, read when a client comes */
  int restart_due, restarting;     /* RISC OS about to reset
                                      (Service_PreReset, 0.2-test24) */
  int restart_state;               /* 1 sending the page, 2 our side shut
                                      (FIN), 3 done */
  unsigned restart_at;
  int shutdown_done;               /* Service_ShutdownComplete: the desktop
                                      has shut down, the Restart box is up */
  int restart_by_input;            /* the page is for a click or key that
                                      may be that box's Restart (0.2-test25) */
  int restart_ll;                  /* which: &70-&72 a button (a whole
                                      click), else a key (down and up) */
  unsigned fills_sent;             /* solid rectangles sent as fills */
  const char *ph_name;             /* what the step is doing (phase ()) */
  unsigned ph_t, ph_y, slow_said;
  int running;
  /* keep-alive (keepalive_step) */
  int ka_caps;                     /* libxrdp_keepalive_caps: 1 heartbeats, 2 RTT */
  int ka_resizing;                 /* keepalive_step saw a resize under way */
  unsigned ka_next;                /* the next heartbeat and RTT request */
  unsigned last_heard;             /* the last data from the client */
  int rtt_seq, rtt_seen;           /* requests sent; replies counted */
  int rtt_ok;                      /* it has answered one: it answers them */
  unsigned rtt_sent_at;
  struct trans *waiting;           /* a new connection, held while the
                                      client is checked (conn_in) */
  unsigned waiting_until;
  unsigned check_from, check_until; /* the check: the client must answer */
  int check_alive;                 /* it did: the waiting connection is refused */
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
/* Keep-alive, for clients that take it (libxrdp patch 0005: the MCS
   message channel). Every KA_PERIOD_S a Server Heartbeat PDU ([MS-RDPBCGR]
   2.2.16.1): if they stop coming (this computer hung, the network went),
   the client warns after KA_WARN missed and connects again by itself
   (auto-reconnect) after KA_RECONNECT. And an RTT Measure Request (2.2.14):
   a client that answers them and then says nothing at all for SILENT_CS
   has gone without closing its connection (asleep, off the network), and
   the session ends, so it can connect again. */
#define KA_PERIOD_S 5
#define KA_WARN 3
#define KA_RECONNECT 6
#define SILENT_CS 4500
/* A connection that comes while a client is connected waits this long
   while that client is checked: if it doesn't answer within CHECK_CS (or
   its connection turns out to be dead), it's dropped and the new one
   takes its place. */
#define WAITING_CS 700
#define CHECK_CS 500
/* A client that takes nothing of what's waiting to go to it for this long
   has gone, even if its connection is still open (a RISC OS client whose
   program stopped: its socket stays, nobody reads it). */
#define STALL_CS 6000
/* The login screen: this long to log in, this many tries, and a pause
   after each wrong one. */
#define LOGIN_CS (2 * 60 * 100)
#define LOGIN_TRIES 5
#define LOGIN_PAUSE_CS 300

static void server_arc_connection_ended (void);
static server srv;
static void phase (server *s, const char *name);
static void updates_say (server *s);

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
  if (plat_click (ll, (unsigned) s->input.click_cs) != 0)
    return -1;
  if (s->opt->verbose)
    say ("click &%02X: KeyV down, up in %d cs by ticker", ll, s->input.click_cs);
  return 0;
}

static int
sink_text (void *ctx, int c)
{
  (void) ctx;
  return plat_char (c);
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
  if ((s->restarting || s->restart_due)
      && (msg == RDP_INPUT_SYNCHRONIZE || msg == RDP_INPUT_SCANCODE
          || msg == RDP_INPUT_UNICODE || msg == RDP_INPUT_MOUSE
          || msg == RDP_INPUT_MOUSEX))
    return 0;                   /* RISC OS is about to reset: no more input */
  /* The desktop has shut down and its box offers only Restart: a click
     or Return from the client is most likely that, and the reset would
     come before the page could go. So this one is held back (its up and
     anything after it dropped), the page goes, then it goes as a whole
     click (or Return), then our side of the connection is shut.
     (0.2-test25) */
  if (s->shutdown_done && s->was_up && !s->in_login
      && ((msg == RDP_INPUT_MOUSE && ((int) param3 & PTRFLAGS_DOWN)
           && ((int) param3 & (PTRFLAGS_BUTTON1 | PTRFLAGS_BUTTON2 | PTRFLAGS_BUTTON3)))
          || (msg == RDP_INPUT_SCANCODE && !((int) param3 & KBD_FLAG_UP) && param1 == 0x1C)))
    {
      int f = (int) param3;
      if (msg == RDP_INPUT_SCANCODE)
        s->restart_ll = keymap_lookup (0x1C, (f & KBD_FLAG_EXT) != 0);
      else
        s->restart_ll = (f & PTRFLAGS_BUTTON1) ? 0x70
                        : (f & PTRFLAGS_BUTTON3) ? 0x71 : 0x72;   /* Select, Menu, Adjust */
      if (msg == RDP_INPUT_MOUSE)
        mouse (s, (int) param1, (int) param2, f & ~(PTRFLAGS_DOWN | PTRFLAGS_BUTTON1
                                                    | PTRFLAGS_BUTTON2 | PTRFLAGS_BUTTON3));
      s->restart_due = 1;
      s->restart_by_input = 1;
      return 0;
    }
  s->last_activity = plat_time_cs ();
  switch (msg)
    {
    case RDP_INPUT_SCANCODE:
      if (s->in_login)
        {
          /* to the login screen, not RISC OS (and not logged: a password) */
          if (login_scancode (&s->login, (int) param1, ((int) param3 & KBD_FLAG_EXT) != 0,
                              ((int) param3 & KBD_FLAG_UP) != 0) == LOGIN_SUBMIT)
            s->login_due = 1;
          break;
        }
      scancode (s, (int) param3, (int) param1);
      break;
    case RDP_INPUT_UNICODE:
      if (s->in_login)
        {
          if (login_unicode (&s->login, (unsigned) param1, ((int) param3 & KBD_FLAG_UP) != 0)
              == LOGIN_SUBMIT)
            s->login_due = 1;
          break;
        }
      /* A character sent as itself, not as a key (Windows App on a Mac,
         an on-screen keyboard, an input method): typed into the keyboard
         buffer, in its turn with the keys. Before 0.2-test23 these were
         dropped outside the login screen. (Which character isn't
         logged.) */
      if (!((int) param3 & KBD_FLAG_UP))
        {
          unsigned char b[4];
          int n = kbd_unicode_bytes ((unsigned) param1, s->alphabet, b), i;
          if (n > 0)
            {
              if (!s->unicode_said)
                say ("the client sends characters as themselves (Unicode): typing them "
                     "into the keyboard buffer (RISC OS alphabet %d)", s->alphabet);
              s->unicode_said = 1;
              for (i = 0; i < n; i++)
                inq_char (&s->input, b[i]);
            }
          else if (!s->unicode_bad_said)
            {
              s->unicode_bad_said = 1;
              say ("the client sent a character RISC OS's alphabet hasn't got: not typed");
            }
        }
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
      if (s->in_login)
        {
          if (((int) param3 & PTRFLAGS_BUTTON1) && ((int) param3 & PTRFLAGS_DOWN)
              && login_click (&s->login, (int) param1, (int) param2) == LOGIN_SUBMIT)
            s->login_due = 1;
          break;
        }
      mouse (s, (int) param3, (int) param1, (int) param2);
      break;
    case 0x5559:   /* output suppressed (param1 1) or allowed again (0) */
      if (param1 == 0 && s->cap.frame)
        {
          cap_mark_all (&s->cap);
          s->palette_due = s->client_bpp == 8;
          s->pointer_due = 1;
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

/* RDPServer$SendBuffer (KB, 16 to 1024): the client's socket send buffer
   to ask for instead of 64 KB, for trying others (0.2-test19). */
static int
sndbuf_from_var (void)
{
  int k = plat_var_int ("RDPServer$SendBuffer", 0);
  return k >= 16 && k <= 1024 ? k * 1024 : 0;
}

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
  s->pal_n = 0;
  if (s->in_login)
    {
      if (s->login_tries > 0)
        s->refuse_until = plat_time_cs () + 300;
      login_close (&s->login);
    }
  s->go_due = 0;
  s->in_login = s->login_due = 0;
  s->ka_caps = 0;
  s->check_alive = 0;
  s->ka_resizing = 0;
  s->rtt_ok = 0;
  s->check_until = 0;
  server_arc_connection_ended ();
  plat_set_clients (0);
  s->client_desc[0] = s->client_user[0] = 0;
  say ("client gone (%s)", why);
  rdps_sndbuf_want = sndbuf_from_var ();     /* for the next one */
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

/* In a palette mode (2 to 256 colours), reads the palette; if it changed
   (a program set its own colours, or the mode changed), everything is
   sent again in the new colours. Not while output is redirected to a
   sprite: the palette read would be the sprite's. */
static void
palette_refresh (server *s, int redirected)
{
  uint32_t now[256];
  int n;
  if (!pix_is_palette (s->fmt) || redirected)
    return;
  memset (now, 0, sizeof now);
  n = plat_palette (now);
  if (n <= 0)
    return;
  if (n == s->pal_n && memcmp (now, s->pal, (size_t) n * sizeof *now) == 0)
    return;
  memcpy (s->pal, now, sizeof now);
  pix_set_palette (s->pal, n);
  if (s->cap.frame)
    {
      cap_mark_all (&s->cap);
      if (s->opt->verbose)
        say ("the palette changed: the screen is sent again");
    }
  s->pal_n = n;
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

/* Keep-alive from now on (see keepalive_step). */
static void
keepalive_start (server *s)
{
  struct xrdp_client_info *ci = s->session->client_info;
  s->ka_caps = libxrdp_keepalive_caps (s->session);
  s->ka_next = s->last_heard = plat_time_cs ();
  s->rtt_seen = ci->rtt_replies;
  s->rtt_seq = 0;
  say ("keep-alive: %s", s->ka_caps == 3 ? "heartbeats, and network checks"
       : s->ka_caps == 1 ? "heartbeats (this client doesn't answer network checks)"
       : s->ka_caps == 2 ? "network checks (this client doesn't take heartbeats)"
       : "none (this client takes neither heartbeats nor network checks)");
}

/* Logged in: the RISC OS screen from now on (s->cap already set up for
   it), sound, the auto-reconnect cookie. */
static void
session_go (server *s, const char *user)
{
  struct xrdp_client_info *ci = s->session->client_info;
  copy_text (s->client_user, sizeof s->client_user, user);
  arc_send (s, ci);
  phase (s, "starting the sound");
  if (s->send_sound && !s->sound_closed)
    sound_start (&s->snd, s->session, s->mute_local, s->opt->verbose, say);
  else if (libxrdp_get_channel_id (s->session, "rdpsnd") >= 0)
    say ("sound: not sent (Choices: \"Send this computer's sound to the client\" is off)");
  s->last_activity = plat_time_cs ();
  say ("client up: %d x %d, %d bpp, user \"%s\"", s->scr.width, s->scr.height,
       s->client_bpp, user);
  s->up_since = plat_time_cs ();
  s->pointer_due = 1;
  plat_set_clients (1);
}

static int screen_resized (server *s);

/* The login screen's user name and password, when it asks (from step:
   ending the session or resizing the client isn't done in libxrdp's
   callback). */
static void
login_try (server *s)
{
  unsigned now = plat_time_cs ();
  char user[LOGIN_FIELD];
  s->login_due = 0;
  if ((int) (now - s->login_wait) < 0)
    {
      login_message (&s->login, "Wait a moment, then try again");
      return;                   /* the pause after a wrong one */
    }
  if (!auth_check (&s->logins, s->login.user, s->login.pass))
    {
      s->login_tries++;
      /* not the name typed: it might be the password, in the wrong field */
      say ("login refused at the login screen (try %d of %d)", s->login_tries, LOGIN_TRIES);
      if (s->login_tries >= LOGIN_TRIES)
        {
          s->refuse_until = now + 300;
          session_end (s, "too many wrong logins");
          return;
        }
      login_message (&s->login, "Wrong user name or password");
      login_clear_password (&s->login);
      s->login_wait = now + LOGIN_PAUSE_CS;
      return;
    }
  copy_text (user, sizeof user, s->login.user);
  login_close (&s->login);
  s->in_login = 0;
  say ("logged in at the login screen as \"%s\"", user);
  /* the name the auto-reconnect cookie is for */
  copy_text (s->session->client_info->username, sizeof s->session->client_info->username, user);
  /* the RISC OS screen now: as it is (it may have changed meanwhile) */
  {
    rdps_screen cur;
    pix_format fmt;
    if (screen_read (&cur) != NULL)
      {
        session_end (s, "can't read the screen");
        return;
      }
    fmt = pix_format_of (cur.log2bpp, cur.ncolour, cur.modeflags);
    if (fmt == PIX_UNSUPPORTED)
      {
        say ("the screen mode: Log2BPP %d, NColour %d, ModeFlags &%X", cur.log2bpp,
             cur.ncolour, cur.modeflags);
        session_end (s, "this screen mode can't be shared");
        return;
      }
    if (cur.width == s->scr.width && cur.height == s->scr.height)
      {
        cap_free (&s->cap);
        s->scr = cur;
        s->fmt = fmt;
        s->pal_n = 0;
        palette_refresh (s, 0);
        if (cap_init (&s->cap, cur.width, cur.height, pix_bits (fmt)) != 0)
          {
            session_end (s, "out of memory for the screen");
            return;
          }
        cap_scan (&s->cap, s->scr.base, s->scr.stride, s->scr.height);
        cap_mark_all (&s->cap);
      }
    else
      {
        if (screen_resized (s) != 0)
          return;               /* it said why and ended the session */
        if (s->resize_deadline)
          {
            /* the client is being resized: sound and the cookie once
               it's back (step) */
            s->go_due = 1;
            copy_text (s->client_user, sizeof s->client_user, user);
            return;
          }
      }
  }
  session_go (s, user);
}

static int
session_up (server *s)
{
  struct xrdp_client_info *ci = s->session->client_info;
  int need_login = 0, wrong = 0;

  if (s->in_login)
    {
      login_close (&s->login);
      s->in_login = 0;
    }
  /* the client's user name and password come as UTF-8; RISC OS (and so
     the login set in Choices) types Latin-1 */
  auth_utf8_to_latin1 (ci->username);
  auth_utf8_to_latin1 (ci->password);

  /* The login, before the client is shown anything: the password, or
     this server's auto-reconnect cookie. Without them, the login screen
     instead of the RISC OS screen. */
  {
    int reconnect = arc_check (ci);
    if (reconnect)
      say ("auto-reconnect: \"%s\" is back (its cookie checked)", ci->username);
    if (s->logins.count > 0 && !reconnect
        && !auth_check (&s->logins, ci->username, ci->password))
      {
        need_login = 1;
        wrong = ci->password[0] != 0;
        if (wrong)
          s->refuse_until = plat_time_cs () + 300;   /* as before: no quick guessing */
        say ("the login screen for \"%s\" (%s)", ci->username,
             wrong ? "a wrong password sent" : "no password sent");
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
    s->pal_n = 0;
    palette_refresh (s, 0);
  }
  if (s->trans && s->trans->tls)
    say ("security: TLS (%s, %s); the handshake's longest step took %u cs", s->trans->ssl_protocol
         ? s->trans->ssl_protocol : "?", s->trans->cipher_name ? s->trans->cipher_name : "?",
         rdps_tls_handshake_cs);
  else
    say ("security: Standard RDP Security (RC4): the client didn't ask for TLS");
  if (s->opt->verbose)
    {
      if (!(s->trans && s->trans->tls))
        say ("key exchange: the RSA step took %u cs", rdps_modexp_cs);
      if (rdps_sndbuf)
        say ("send buffer: %d KB", rdps_sndbuf / 1024);
      else
        say ("send buffer: the default (a bigger one was refused)");
    }
  s->client_bpp = ci->bpp;
  rdps_orders_reset (s->session);
  updates_say (s);      /* fills or not, before the login screen too */
  s->tilebuf = malloc ((size_t) CAP_TILE * CAP_TILE * 4);
  if (!s->tilebuf)
    return -1;
  if (s->client_bpp == 8)
    {
      int pal[256];
      pix_palette_332 (pal);
      libxrdp_send_palette (s->session, pal);
    }
  keepalive_start (s);
  if (need_login)
    {
      /* the login screen: the client sees only that, the RISC OS screen
         isn't read */
      unsigned now = plat_time_cs ();
      /* typing with RISC OS's keyboard layout, as in the session */
      int kb = plat_keyboard (), use = kb;
      if (!kbd_known (use))
        use = kbd_from_client (ci->keylayout);
      if (!kbd_known (use))
        use = KBD_USA;
      say ("the login screen types with the %s keyboard layout (RISC OS keyboard %d%s)",
           kbd_name (use), kb, use == kb ? "" : ", which it has no table for");
      if (login_open (&s->login, s->scr.width, s->scr.height, ci->username, use) != 0)
        return -1;
      s->in_login = 1;
      if (wrong)
        login_message (&s->login, "Wrong user name or password");
      s->login_tries = wrong;
      s->login_wait = now + (wrong ? LOGIN_PAUSE_CS : 0);
      s->login_until = now + LOGIN_CS;
      if (cap_init (&s->cap, s->login.w, s->login.h, 32) != 0)
        return -1;
      cap_scan (&s->cap, (const unsigned char *) s->login.px, (size_t) s->login.w * 4, s->login.h);
      cap_mark_all (&s->cap);
      s->pointer_due = 1;
      s->last_activity = now;
      copy_text (s->client_user, sizeof s->client_user, "");
      return 0;
    }
  if (cap_init (&s->cap, s->scr.width, s->scr.height, pix_bits (s->fmt)) != 0)
    return -1;
  phase (s, "starting the session: reading the whole screen");
  cap_scan (&s->cap, s->scr.base, s->scr.stride, s->scr.height);
  cap_mark_all (&s->cap);
  session_go (s, ci->username);
  return 0;
}

/* Solid areas go as OpaqueRect orders ([MS-RDPEGDI] 2.2.2.2.1.1.2.5), a
   few bytes, instead of a compressed bitmap, if the client takes them
   (orderSupport[TS_NEG_PATBLT_INDEX], which covers OpaqueRect).
   At 32 bpp, only to RDP 10 clients: the colour is red, green, blue from
   the lowest byte up at 24 and 32 bpp (as xrdp sends it, and FreeRDP and
   Windows take it), but rdesktop (and so RISC OS's RDPClient) takes it
   the other way round at 32 bpp, and says it's RDP 5, as Windows 7 and 8
   do: those get bitmaps at 32 bpp, which are right everywhere.
   (0.2-test22) */
static void
updates_say (server *s)
{
  struct xrdp_client_info *ci = s->session->client_info;
  int old32 = s->client_bpp == 32 && (unsigned) ci->rdp_client_version < 0x00080005u;
  s->fills_ok = s->fills_var != 0 && ci->orders[TS_NEG_PATBLT_INDEX] != 0 && !old32;
  say ("updates (client version &%08X): %s bitmaps, %s; solid areas as %s",
       (unsigned) ci->rdp_client_version, ci->use_bitmap_comp ? "compressed" : "uncompressed",
       ci->rdp_compression ? "bulk compressed (MPPC)" : "not bulk compressed",
       s->fills_ok ? "fills (OpaqueRect)"
       : s->fills_var == 0 ? "bitmaps (RDPServer$Fills)"
       : old32 ? "bitmaps (at 32 bpp, fills go to RDP 10 clients only)"
       : "bitmaps (the client takes no fills)");
}

static void send_dirty (server *s);

/* RISC OS is about to reset (Service_PreReset, from the module): the
   client gets a page saying so in the screen's place, and nothing more;
   then the module closes the connection as if the network had gone, so
   the client reconnects by itself once the computer is back (if
   RDPServer starts with it), showing the page meanwhile. (0.2-test24) */
static int sink_click (void *ctx, int ll);

/* The click (or Return) held back for the restarting page, now. */
static void
restart_deliver (server *s)
{
  int ll = s->restart_ll;
  if (!s->restart_by_input || ll < 0)
    return;
  s->restart_ll = -1;
  inq_run (&s->input, plat_time_cs (), &s->sink);     /* the pointer's move first */
  if (ll >= 0x70 && ll <= 0x72)
    {
      if (sink_click (s, ll) != 0)
        {
          plat_key (ll, 1);
          plat_key (ll, 0);
        }
    }
  else
    {
      plat_key (ll, 1);
      plat_key (ll, 0);
    }
}

static void
restart_notice (server *s)
{
  s->restarting = 1;
  s->restart_state = 3;         /* (unless there's a page to send, below) */
  s->restart_at = plat_time_cs ();
  if (!s->was_up || !s->session || !s->trans || !s->tilebuf
      || s->session->client_info->suppress_output_mask)
    {
      restart_deliver (s);
      return;
    }
  login_close (&s->login);
  if (login_notice (&s->login, s->scr.width, s->scr.height, "RISC OS is restarting",
                    "Connect again once it's back") != 0)
    {
      restart_deliver (s);
      return;
    }
  cap_free (&s->cap);
  if (cap_init (&s->cap, s->login.w, s->login.h, 32) != 0)
    {
      restart_deliver (s);
      return;
    }
  s->in_login = 1;              /* (send_dirty: the page, not the screen) */
  cap_scan (&s->cap, (const unsigned char *) s->login.px, (size_t) s->login.w * 4, s->login.h);
  cap_mark_all (&s->cap);
  s->restart_state = 1;
  send_dirty (s);
  say ("RISC OS is restarting: the client was told so");
}

/* Sends the changed tiles. A whole screen takes a while to compress (the
   first frame, or everything changing at once), so every 2 cs the
   thread lets other programs run (srvthread.h): on RISC OS the desktop
   never stops for more than about that. */
#define SEND_SLICE_CS 2

static void
send_dirty (server *s)
{
  int i = 0, x, y, w, h, orders_open = 0, color;
  unsigned slice = plat_time_cs ();
  const char *was = s->ph_name;
  phase (s, "sending the screen");
  if (s->palette_due)
    {
      int pal[256];
      pix_palette_332 (pal);
      libxrdp_send_palette (s->session, pal);
      s->palette_due = 0;
    }
  if (s->pointer_due)
    {
      static unsigned char data[PTR_SIZE * PTR_SIZE * 3], mask[PTR_SIZE * PTR_SIZE / 8];
      int hx, hy;
      s->pointer_due = 0;
      pointer_arrow (data, mask, &hx, &hy);
      if (libxrdp_send_pointer (s->session, 0, (char *) data, (char *) mask, hx, hy,
                                24, PTR_SIZE, PTR_SIZE) != 0)
        say ("couldn't send the pointer's shape");
    }
  while (cap_next_dirty (&s->cap, &i, &x, &y, &w, &h))
    {
      if ((int) (plat_time_cs () - slice) >= SEND_SLICE_CS)
        {
          /* the fills so far go before the client waits on us */
          if (orders_open)
            {
              libxrdp_orders_send (s->session);
              orders_open = 0;
            }
          rdps_wait_cs (0);
          slice = plat_time_cs ();
          if (!s->session || !s->trans || rdps_thread_ending ())
            return;
        }
      pix_convert (s->in_login ? PIX_32_TBGR : s->fmt, cap_pixel (&s->cap, x, y),
                   s->cap.row_bytes, w, h,
                   s->client_bpp, s->tilebuf);
      /* Tiles don't overlap, so the fills can wait in one orders PDU
         while bitmaps of other tiles go. */
      if (s->fills_ok && pix_solid (s->tilebuf, w, h, s->client_bpp, &color))
        {
          if (!orders_open)
            {
              orders_open = libxrdp_orders_init (s->session) == 0;
              if (!orders_open)
                {
                  /* (its count went up all the same) */
                  libxrdp_orders_send (s->session);
                  s->fills_ok = 0;
                  say ("couldn't start a fill: sending bitmaps instead");
                }
            }
          if (orders_open && libxrdp_orders_rect (s->session, x, y, w, h, color, NULL) == 0)
            {
              s->fills_sent++;
              s->tiles_sent++;
              continue;
            }
        }
      libxrdp_send_bitmap (s->session, w, h, s->client_bpp,
                           (char *) s->tilebuf, x, y, w, h);
      s->tiles_sent++;
    }
  if (orders_open)
    libxrdp_orders_send (s->session);
  phase (s, was);
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
      say ("this screen mode can't be shared (Log2BPP %d, NColour %d, ModeFlags &%X)",
           s->scr.log2bpp, s->scr.ncolour, s->scr.modeflags);
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
  /* Choices' "only allow TLS": as security_layer=tls in xrdp.ini, so a
     client that doesn't ask for TLS is told the server requires it */
  if (s->require_tls)
    s->session->client_info->security_layer = SECURITY_LAYER_TLS;
  if (s->bulk_var == 0)
    s->session->client_info->use_bulk_comp = 0;
  t->si = &s->session->si;
  t->my_source = XRDP_SOURCE_CLIENT;
  s->session->callback = callback;
  s->session->is_term = is_term;
  g_sck_get_peer_description ((int) t->sck, s->client_desc, sizeof s->client_desc);
  s->client_user[0] = 0;
  s->sound_closed = s->sound_stop_due = 0;
  s->unicode_said = s->unicode_bad_said = 0;
  s->alphabet = plat_alphabet ();
  say ("connection from %s", s->client_desc);
  /* The connection sequence, up to the client's capabilities. */
  if (libxrdp_process_incoming (s->session) != 0)
    {
      int why = rdps_negotiation_failure (s->session);
      session_end (s, rdps_thread_ending () ? "disconnected from this computer"
                      : is_term () && !server_stop ? "too slow connecting"
                      : why == SSL_REQUIRED_BY_SERVER ? "refused: it didn't ask for TLS, "
                        "and only TLS connections are allowed (Choices)"
                      : why == SSL_CERT_NOT_ON_SERVER ? "refused: TLS is required, but "
                        "there's no certificate (Choices:RDPServer.cert.pem)"
                      : "the connection sequence failed");
      return;
    }
  init_stream (t->in_s, 32 * 1024);
}

/* Asks the connected client for an answer now. One that answers network
   checks gets an RTT request and has CHECK_CS to reply (or send anything
   else). For one that doesn't, a little of the screen is sent again: if
   its end of the connection has gone, its computer answers with a reset,
   and the session ends on that. */
static void waiting_step (server *s, unsigned now);

static void
check_client (server *s)
{
  unsigned now = plat_time_cs ();
  s->check_from = now;
  s->check_until = now + CHECK_CS;
  s->check_alive = 0;
  if (s->resize_deadline)
    return;                     /* keepalive_step decides */
  if (s->rtt_ok)
    {
      if (libxrdp_send_rtt_request (s->session, ++s->rtt_seq & 0xffff) == 0)
        s->rtt_sent_at = now;
    }
  else if (s->cap.dirty)
    /* the whole screen again: a client that's there takes it at once; one
       that isn't reading leaves it waiting (keepalive_step), and one whose
       end has gone answers with a reset */
    cap_mark_all (&s->cap);
}

static int
conn_in (struct trans *listener, struct trans *t)
{
  server *s = (server *) listener->callback_data;
  if ((s->refuse_until && (int) (plat_time_cs () - s->refuse_until) < 0)
      || s->restarting || s->restart_due)
    {
      trans_delete (t);
      return 0;
    }
  if (s->trans == NULL && s->waiting != NULL)
    /* the client it waited for has gone: it goes first */
    waiting_step (s, plat_time_cs ());
  if (s->trans != NULL)
    {
      char desc[64];
      g_sck_get_peer_description ((int) t->sck, desc, sizeof desc);
      if (s->waiting != NULL || !s->was_up)
        {
          say ("refused %s: a client is already connected", desc);
          trans_delete (t);
          return 0;
        }
      /* One client at a time; but the one there may have gone without
         closing its connection (that computer asleep or off the
         network): check it, and hold this one meanwhile (step). */
      s->waiting = t;
      s->waiting_until = plat_time_cs () + WAITING_CS;
      say ("connection from %s: waiting while the client that's connected "
           "is checked", desc);
      check_client (s);
      return 0;
    }
  session_start (s, t);
  return 0;
}
/* At start-up: how fast this computer encodes the screen as it is, at 32
   and 16 bpp (encspeed.c), for comparing with what the 5-second figures
   show (0.2-test22). About 0.25 s. */
static void
encode_speed_say (void)
{
  rdps_screen scr;
  static const int depths[2] = { 32, 16 };
  int i;
  pix_format f;
  if (plat_screen (&scr) != NULL || scr.redirected)
    return;
  f = pix_format_of (scr.log2bpp, scr.ncolour, scr.modeflags);
  for (i = 0; i < 2; i++)
    {
      rdps_enc_speed r;
      if (rdps_encode_speed (&scr, f, depths[i], 3, &r) != 0)
        {
          if (i == 0)
            say ("encoding here: not measured (a palette mode, or out of memory)");
          return;
        }
      say ("encoding here, a whole screen like its middle rows at %d bpp: converting "
           "%u ms, compressing %u ms (%u KB), MPPC %u ms (%u KB)", depths[i],
           rdps_enc_ms (r.screen_tiles, r.convert_tps), rdps_enc_ms (r.screen_tiles, r.compress_tps),
           r.rle_kb, rdps_enc_ms (r.screen_tiles, r.mppc_tps), r.mppc_kb);
    }
}

/* What the step is doing now, for finding what holds the machine up:
   with -v, a phase that held it for 10 cs or more (not counting the
   waits in it, srvthread.h) is logged, at most 5 times in 5 seconds.
   (0.2-test22) */
#define SLOW_CS 10
static void
phase (server *s, const char *name)
{
  unsigned t = plat_time_cs (), y = rdps_yielded_cs;
  if (s->ph_name)
    {
      unsigned held = (t - s->ph_t) - (y - s->ph_y);
      if (held >= SLOW_CS && (int) held > 0 && s->opt && s->opt->verbose && s->slow_said < 5)
        {
          s->slow_said++;
          say ("slow: %u cs %s", held, s->ph_name);
        }
    }
  s->ph_name = name;
  s->ph_t = t;
  s->ph_y = y;
}

/* 1, 2, 4, 8 or 16: the most of those that isn't over v (1 for less) */
static int
sample_snap (int v)
{
  return v >= 16 ? 16 : v >= 8 ? 8 : v >= 4 ? 4 : v >= 2 ? 2 : 1;
}

/* How fast rows come out of the screen each way (KB/s), copying for cs
   centiseconds into a row buffer (or until 64 passes over the screen, in
   case the clock isn't running). */
static unsigned
screen_copy_speed (const rdps_screen *scr, unsigned char *row, size_t row_bytes,
                   int way, unsigned cs)
{
  unsigned t0 = plat_time_cs (), t;
  unsigned long bytes = 0;
  int y = 0, passes = 0;
  do
    {
      int n;
      for (n = 0; n < 16; n++)
        {
          cap_copy_with (way, row, scr->base + (size_t) y * scr->stride, row_bytes);
          bytes += row_bytes;
          if (++y == scr->height)
            {
              y = 0;
              passes++;
            }
        }
      t = plat_time_cs ();
    }
  while ((int) (t - t0) < (int) cs && passes < 64);
  return t == t0 ? 0 : (unsigned) (bytes / 1024 * 100 / (t - t0));
}

/* At start-up: time copying out of the screen by single words (the
   module's memcpy until 0.2-test21) and by LDM of eight words, say so,
   and use the quicker. *Set RDPServer$ScreenCopy 1 (words) or 2 (LDM 8)
   to choose instead. */
static void
screen_copy_choose (server *s)
{
  rdps_screen scr;
  int forced = plat_var_int ("RDPServer$ScreenCopy", -1);
  unsigned char *row;
  size_t row_bytes;

  (void) s;
  if (plat_screen (&scr) != NULL || scr.redirected || scr.width <= 0 || scr.height <= 0)
    return;
  row_bytes = ((size_t) scr.width << scr.log2bpp) / 8;
  row = malloc (row_bytes + 32);
  if (row != NULL)
    {
      unsigned w = screen_copy_speed (&scr, row, row_bytes, CAP_COPY_WORDS, 10);
      unsigned l = screen_copy_speed (&scr, row, row_bytes, CAP_COPY_LDM8, 10);
      cap_copy_way = l >= w ? CAP_COPY_LDM8 : CAP_COPY_WORDS;
      free (row);
      say ("screen read: single words %u MB/s, LDM 8 %u MB/s", w / 1024, l / 1024);
    }
  if (forced == CAP_COPY_WORDS || forced == CAP_COPY_LDM8)
    cap_copy_way = forced;
  say ("screen read by %s%s", cap_copy_way == CAP_COPY_LDM8 ? "LDM 8" : "single words",
       forced == CAP_COPY_WORDS || forced == CAP_COPY_LDM8 ? " (RDPServer$ScreenCopy)" : "");
}


/* --- the loop ------------------------------------------------------------- */

int
server_start (const server_options *o)
{
  server *s = &srv;

  rdps_tls_forget ();               /* the key and certificate may be new */
  memset (s, 0, sizeof *s);
  rdps_sndbuf_want = sndbuf_from_var ();
  if (rdps_sndbuf_want)
    say ("RDPServer$SendBuffer: asking for a %d KB send buffer", rdps_sndbuf_want / 1024);
  /* RDPServer$ScanRows: the most rows read a turn (0: no limit), for
     trying others; else the options' (0: each turn reads the rows due
     since the last, so a pass takes the scan period). */
  s->scan_rows = plat_var_int ("RDPServer$ScanRows", -1);
  if (s->scan_rows >= 0)
    {
      if (s->scan_rows > 0 && s->scan_rows < 16)
        s->scan_rows = 16;
      if (s->scan_rows > 0)
        say ("RDPServer$ScanRows: reading at most %d rows a turn", s->scan_rows);
      else
        say ("RDPServer$ScanRows: no limit on the rows read a turn");
    }
  else
    s->scan_rows = o->rows_per_scan;
  /* RDPServer$SampleRows: each pass reads 1 row in this many (1, 2, 4, 8
     or 16), and the rest of a tile where one changed (capture.h). 1:
     every row, as before 0.2-test21. */
  s->sample = plat_var_int ("RDPServer$SampleRows", -1);
  if (s->sample >= 0)
    {
      s->sample = sample_snap (s->sample);
      say ("RDPServer$SampleRows: reading %s", s->sample > 1 ? "1 row in" : "every row");
      if (s->sample > 1)
        say ("  %d each pass", s->sample);
    }
  else
    s->sample = o->sample_rows > 0 ? sample_snap (o->sample_rows) : 8;
  /* For comparing (0.2-test22): RDPServer$Fills 0 sends solid areas as
     bitmaps; RDPServer$BulkCompress 0 doesn't MPPC-compress updates. */
  s->fills_var = plat_var_int ("RDPServer$Fills", -1);
  if (s->fills_var == 0)
    say ("RDPServer$Fills: solid areas sent as bitmaps, not fills");
  s->bulk_var = plat_var_int ("RDPServer$BulkCompress", -1);
  if (s->bulk_var == 0)
    say ("RDPServer$BulkCompress: updates not bulk compressed (MPPC)");
  s->snd.chan = -1;
  s->opt = o;
  s->mute_local = o->mute_local;
  s->require_tls = o->require_tls;
  s->send_sound = o->send_sound;
  inq_init (&s->input);
  /* For comparing (0.2-test25): RDPServer$ClickTime, how long a click
     holds the button (cs, 2 to 50; 6); RDPServer$Clicks 0, buttons down
     and up as the client sends them (as before 0.2-test4). */
  {
    int ct = plat_var_int ("RDPServer$ClickTime", -1), cm = plat_var_int ("RDPServer$Clicks", -1);
    if (ct >= 0)
      {
        s->input.click_cs = ct < 2 ? 2 : ct > 50 ? 50 : ct;
        say ("RDPServer$ClickTime: a click holds the button %d cs", s->input.click_cs);
      }
    if (cm == 0)
      {
        s->input.plain = 1;
        say ("RDPServer$Clicks 0: mouse buttons down and up as the client sends them");
      }
  }
  s->sink.move = sink_move;
  s->sink.key = sink_key;
  s->sink.wheel = sink_wheel;
  s->sink.now = sink_now;
  s->sink.click = sink_click;
  s->sink.text = sink_text;
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
  {
    /* TLS: the certificate's fingerprints, to compare with what a client
       shows the first time it connects (the certificate is self-signed:
       only this check tells a client it reached this computer). */
    char cert[300], sha1[64], sha256[100];
    snprintf (cert, sizeof cert, "%s/cert.pem", plat_choices_dir ());
    if (rdps_tls_fingerprints (cert, sha1, sizeof sha1, sha256, sizeof sha256) == 0)
      {
        say ("TLS on: certificate SHA-1 %s", sha1);
        {
          /* how fast this computer encrypts, for comparing with what's sent */
          unsigned cc = rdps_tls_speed (1, 10), gcm = rdps_tls_speed (2, 10);
          say ("TLS here: ChaCha20-Poly1305 encrypts %u.%u MB/s, AES-128-GCM %u.%u MB/s",
               cc / 1024, cc % 1024 * 10 / 1024, gcm / 1024, gcm % 1024 * 10 / 1024);
        }
        say ("TLS certificate SHA-256 %.48s", sha256);
        say ("  ... %s", sha256 + 48);
      }
    else
      say ("TLS off (no certificate): clients get Standard RDP Security only");
  }
  screen_copy_choose (s);
  encode_speed_say ();
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

  phase (s, "a mode change: the new screen");
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
      say ("the new screen mode: Log2BPP %d, NColour %d, ModeFlags &%X",
           cur.log2bpp, cur.ncolour, cur.modeflags);
      session_end (s, "the screen mode changed to one that can't be shared");
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
      s->pal_n = 0;
      palette_refresh (s, 0);
      if (cap_init (&s->cap, cur.width, cur.height, pix_bits (fmt)) != 0)
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
  s->pal_n = 0;
  palette_refresh (s, 0);
  g_rdps_desktop_width = cur.width;
  g_rdps_desktop_height = cur.height;
  ci->display_sizes.session_width = cur.width;
  ci->display_sizes.session_height = cur.height;
  ci->display_sizes.monitorCount = 0;   /* one screen, the new size */
  if (cap_init (&s->cap, cur.width, cur.height, pix_bits (fmt)) != 0)
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
  unsigned active = s->opt->interval_cs ? s->opt->interval_cs : s->sample > 1 ? 5 : 10;
  unsigned idle = s->opt->idle_interval_cs ? s->opt->idle_interval_cs : 25;
  return (int) (now - s->last_activity) < ACTIVE_CS ? active : idle;
}

/* Heartbeats, network checks, and what they show (see KA_PERIOD_S).
   Returns -1 if the session ended. */
static int
keepalive_step (server *s, unsigned now)
{
  struct xrdp_client_info *ci;
  unsigned silent = s->opt->silent_cs ? s->opt->silent_cs : SILENT_CS;

  if (!s->was_up || !s->session || !s->trans)
    return 0;
  if (s->resize_deadline)
    {
      /* Reactivating after a resize: that has its own deadline, and
         nothing else is sent meanwhile. A healthy client is back within
         a second or two; one that a new connection is waiting behind and
         that still isn't back after CHECK_CS has most likely given up
         (and is probably that new connection). */
      unsigned started = s->resize_deadline - HANDSHAKE_CS;
      s->ka_resizing = 1;
      if (s->check_until && (int) (now - started) >= CHECK_CS
          && (int) (now - s->check_from) >= CHECK_CS
          && (int) (s->last_heard - s->check_from) < 0
          && s->session->client_info->suppress_output_mask != 0)
        {
          session_end (s, "a new connection came, and this client hadn't "
                       "come back after the resize");
          return -1;
        }
      return 0;
    }
  if (s->ka_resizing)
    {
      /* back after the resize: the silence so far doesn't count */
      s->ka_resizing = 0;
      s->last_heard = now;
    }
  /* output waiting and nothing going: the client has stopped reading */
  if (s->trans->wait_s == NULL || rdps_bytes_sent != s->stall_bytes)
    {
      s->stall_bytes = rdps_bytes_sent;
      s->stall_since = now;
    }
  else if ((int) (now - s->stall_since)
           >= (int) (s->opt->stall_cs ? s->opt->stall_cs : STALL_CS))
    {
      session_end (s, "the client stopped taking data: nothing went to it for a minute");
      return -1;
    }
  ci = s->session->client_info;
  if (ci->rtt_replies != s->rtt_seen)
    {
      if (s->rtt_seen == 0 || s->opt->verbose)
        say ("network check answered (round trip %u cs)", now - s->rtt_sent_at);
      s->last_rtt_cs = now - s->rtt_sent_at;
      s->rtt_seen = ci->rtt_replies;
      s->rtt_ok = 1;
      s->last_heard = now;
    }
  if (s->ka_caps && (int) (now - s->ka_next) >= 0)
    {
      s->ka_next = now + KA_PERIOD_S * 100;
      if ((s->ka_caps & 1)
          && libxrdp_send_heartbeat (s->session, KA_PERIOD_S, KA_WARN, KA_RECONNECT) < 0)
        {
          session_end (s, "disconnected");
          return -1;
        }
      if (s->ka_caps & 2)
        {
          if (libxrdp_send_rtt_request (s->session, ++s->rtt_seq & 0xffff) < 0)
            {
              session_end (s, "disconnected");
              return -1;
            }
          s->rtt_sent_at = now;
        }
    }
  /* only for a client that has shown it answers (some may say they take
     network checks and not answer these) */
  if (s->rtt_ok && (int) (now - s->last_heard) >= (int) silent)
    {
      session_end (s, "the client stopped answering: nothing from it for a while");
      return -1;
    }
  if (s->check_until)
    {
      if ((int) (s->last_heard - s->check_from) >= 0)
        {
          s->check_until = 0;   /* it answered */
          s->check_alive = 1;
        }
      else if ((int) (now - s->check_until) >= 0)
        {
          s->check_until = 0;
          if (s->rtt_ok)
            {
              session_end (s, "a new connection came, and this client didn't "
                           "answer a network check");
              return -1;
            }
          /* without network checks: the whole screen was sent again
             (check_client); still waiting to go, the client isn't reading */
          if (s->trans->wait_s != NULL)
            {
              session_end (s, "a new connection came, and this client wasn't "
                           "taking what was sent to it");
              return -1;
            }
          s->check_alive = 1;   /* it took it all */
        }
    }
  return 0;
}

/* The connection held by conn_in: started once the client before it has
   gone, refused if that one is still there when its time is up. */
static void
waiting_step (server *s, unsigned now)
{
  struct trans *t = s->waiting;
  if (t == NULL)
    return;
  if (s->trans == NULL)
    {
      s->waiting = NULL;
      say ("the waiting connection goes ahead");
      session_start (s, t);
      return;
    }
  if ((int) (now - s->waiting_until) >= 0 || s->check_alive)
    {
      char desc[64];
      s->waiting = NULL;
      say ("refused %s: the client that's connected is still there",
           g_sck_get_peer_description ((int) t->sck, desc, sizeof desc));
      trans_delete (t);
    }
}

static int
step (unsigned max_wait_cs)
{
  server *s = &srv;
  fd_set rd, wr;
  struct timeval tv;
  int backlog, maxfd, tls_more;
  unsigned now = plat_time_cs (), wait_cs;

  if (!s->running)
    return 25;
  phase (s, "reading from the client");
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
  /* TLS may already have read more from the socket than it has given
     out: the socket shows nothing, but there's more to take now. */
  tls_more = s->trans && s->trans->tls && rdps_tls_pending (s->trans->tls);
  if (tls_more)
    wait_cs = 0;
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
  if (s->trans && (FD_ISSET ((int) s->trans->sck, &rd) || tls_more
                   || (backlog && FD_ISSET ((int) s->trans->sck, &wr))))
    {
      unsigned until = plat_time_cs () + 3;
      int n = 0;
      /* anything from the client counts as hearing from it (room to send
         more doesn't: the stack may take a lot for a client that's gone
         before its buffers are full) */
      if (FD_ISSET ((int) s->trans->sck, &rd) || tls_more)
        s->last_heard = plat_time_cs ();
      do
        {
          if (trans_check_wait_objs (s->trans) != 0)
            {
              session_end (s, "disconnected");
              return 0;
            }
          n++;
        }
      while (n < 1000 && (int) (plat_time_cs () - until) < 0 && s->trans
             && s->trans->trans_can_recv (s->trans, (int) s->trans->sck, 0));
      s->reads += (unsigned) n;
      if ((unsigned) n > s->most_reads)
        s->most_reads = (unsigned) n;
    }
  if (s->restart_due)
    {
      s->restart_due = 0;
      restart_notice (s);
      return 1;
    }
  if (s->restarting)
    {
      /* Once libxrdp has handed the page to the socket, shut our side
         (a FIN after it, from here, where the Internet module's
         callbacks still run), then give it a moment before the module
         closes everything. Nothing else until the reset. */
      unsigned t = plat_time_cs ();
      if (s->restart_state == 1 && (!s->trans || s->trans->wait_s == NULL))
        {
          /* the page is out: now the click (or Return) that may be
             Restart */
          restart_deliver (s);
          if (s->trans)
            shutdown ((int) s->trans->sck, 1);   /* SHUT_WR */
          s->restart_state = 2;
          s->restart_at = t;
        }
      else if (s->restart_state == 2 && (!s->trans || (int) (t - s->restart_at) >= 10))
        s->restart_state = 3;
      else if (s->restart_state == 3 && s->restart_by_input && (int) (t - s->restart_at) >= 500)
        {
          /* it wasn't Restart: the client can connect again */
          s->restarting = s->restart_by_input = s->restart_state = 0;
          say ("no restart followed the page: clients can connect again");
          if (s->trans)
            session_end (s, "the restarting page was sent, but RISC OS didn't restart");
          return 0;
        }
      return 1;
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

  phase (s, "starting the session");
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
      s->scan_at = plat_time_cs ();
    }
  else if (s->session && !s->session->up_and_running && s->was_up)
    {
      /* deactivated (a reset): the client sent its logon info again. Not
         at the login screen: that would start it afresh. */
      if (s->in_login)
        {
          s->refuse_until = plat_time_cs () + 300;
          session_end (s, "the client sent its login again at the login screen");
          return 0;
        }
      /* otherwise wait to be up again, but not for ever */
      s->handshake_deadline = plat_time_cs ()
                              + (s->opt->handshake_cs ? s->opt->handshake_cs : HANDSHAKE_CS);
      s->was_up = 0;
      cap_free (&s->cap);
      free (s->tilebuf);
      s->tilebuf = NULL;
    }

  now = plat_time_cs ();
  if (s->sound_stop_due)
    {
      s->sound_stop_due = 0;
      if (s->snd.stage > 0)
        {
          sound_stop (&s->snd, s->session != NULL);
          s->sound_closed = 1;
          say ("sound: stopped (Choices)");
        }
    }
  phase (s, "keep-alive");
  if (keepalive_step (s, now) != 0)
    {
      waiting_step (s, plat_time_cs ());
      return 0;
    }
  waiting_step (s, now);
  /* Sound first: it goes on while the client is minimised (Suppress
     Output), not while it's reactivating after a resize. */
  if (s->was_up && s->session && s->trans && s->trans->wait_s == NULL
      && !s->resize_deadline)
    {
      phase (s, "sound");
      sound_step (&s->snd);
    }
  /* While the client hasn't taken what we sent, don't send more: the
     changes wait as dirty tiles, so a slow link gets fewer, later frames
     rather than an ever-growing queue. */
  /* The login screen's time limit holds while minimised too. */
  if (s->was_up && s->in_login && (int) (now - s->login_until) >= 0)
    {
      session_end (s, "nobody logged in at the login screen");
      return 0;
    }
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
  if (s->go_due && s->session)
    {
      char user[64];
      s->go_due = 0;
      copy_text (user, sizeof user, s->client_user);
      session_go (s, user);
    }
  phase (s, "the login screen, or reading the screen");
  if (s->was_up && s->in_login)
    {
      /* the login screen in the RISC OS screen's place */
      if (s->login_due)
        {
          login_try (s);
          if (!s->trans)
            return 0;
        }
      if (s->in_login)
        {
          if (s->trans->wait_s == NULL && (int) (now - s->next_scan) >= 0)
            {
              /* the page is compared only after it changed */
              if (s->login.changed)
                {
                  s->login.changed = 0;
                  cap_scan (&s->cap, (const unsigned char *) s->login.px,
                            (size_t) s->login.w * 4, s->login.h);
                }
              send_dirty (s);
              s->next_scan = now + 5;
            }
          return 2;
        }
      now = plat_time_cs ();
    }
  if (s->was_up && s->trans->wait_s == NULL && (int) (now - s->next_scan) >= 0)
    {
      rdps_screen cur;
      unsigned period = scan_period (s, now);
      int band, h = s->scr.height;
      /* A mode change moves or resizes the screen: stop reading the old
         one, and give the client the new size (or end the session). */
      const char *e = plat_screen (&cur);
      int redirected = e == NULL && cur.redirected;
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
      /* A pass over the screen: its time, and a program may have changed
         the palette. A band never runs past the bottom, so every pass
         starts at row 0. (0.2-test19 waited for row 0 with bands that
         didn't divide the height: with 128-row bands and 1200 rows it
         came round only every 7.5 passes.) */
      if (s->cap.next_row == 0)
        {
          if (s->pass_start)
            s->pass_cs = now - s->pass_start;
          s->pass_start = now;
          palette_refresh (s, redirected);
        }
      /* The rows due since the last band, so a pass takes the scan
         period however often turns come (before 0.2-test21: a fixed 128
         rows a turn, and with a turn every 2 cs a pass took 20 cs, not
         10); at most scan_rows if that's set, and not past the bottom. */
      {
        unsigned since = now - s->scan_at;
        band = since >= period ? h : (int) ((unsigned long) h * since / period);
        if (band < 16)
          band = 16;
        if (s->scan_rows > 0 && band > s->scan_rows)
          band = s->scan_rows;
        /* Reading every row, a whole screen at once after a gap would
           hold the desktop up: a quarter at most. */
        else if (s->scan_rows == 0 && s->sample <= 1 && band > h / 4 && h >= 64)
          band = h / 4;
        if (band > h - s->cap.next_row)
          band = h - s->cap.next_row;
      }
      s->scan_at = now;
      s->scans++;
      s->cap.sample = s->sample;
      if (cap_scan_sampled (&s->cap, s->scr.base, s->scr.stride, band) > 0)
        s->last_activity = now;
      send_dirty (s);
      /* about a quarter of a pass a turn: every cs while changing, every
         6 cs when still */
      s->next_scan = now + (period >= 8 ? period / 4 : 1);
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
  phase (s, NULL);
  /* more already read by TLS: no socket event will say so */
  if (r > 0 && s->trans && s->trans->tls && rdps_tls_pending (s->trans->tls))
    r = 0;

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
             "longest step %u cs, longest wait %u cs, %u tiles sent in all (%u as fills), %u KB/s, "
             "a pass over the screen %u cs, %u KB/s read from the screen",
             s->steps, s->reads, s->most_reads, s->scans, s->step_cs_max,
             s->gap_max, s->tiles_sent, s->fills_sent, (rdps_bytes_sent - s->stats_bytes) / 5 / 1024,
             s->pass_cs, (unsigned) ((s->cap.bytes_read >= s->stats_read
                                      ? s->cap.bytes_read - s->stats_read
                                      : s->cap.bytes_read) / 5 / 1024));
      s->stats_bytes = rdps_bytes_sent;
      s->stats_read = s->cap.bytes_read;
      s->slow_said = 0;
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

/* Choices changed "only allow TLS connections": from the next client. */
void
server_set_require_tls (int on)
{
  srv.require_tls = on != 0;
}

void
server_restarting (void)
{
  /* already on its way for a click after the shutdown (that click was
     Restart): let that finish, don't send the page again */
  if (srv.restarting)
    return;
  srv.restart_due = 1;
  srv.restart_by_input = 0;
}

void
server_shutdown_complete (void)
{
  srv.shutdown_done = 1;
}

/* The page has gone to the network stack, our side shut after it (or
   there was no client to tell). */
int
server_restart_done (void)
{
  server *s = &srv;
  if (s->restart_due)
    return !s->trans || !s->was_up;     /* nobody to tell: don't wait */
  return s->restarting && s->restart_state == 3;
}

/* (Only a flag: called from the front end, it mustn't send anything
   itself; step stops the sound.) */
void
server_set_sound (int on)
{
  server *s = &srv;
  s->send_sound = on != 0;
  if (!s->send_sound && s->snd.stage > 0)
    s->sound_stop_due = 1;
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
  if (s->waiting)
    {
      trans_delete (s->waiting);
      s->waiting = NULL;
    }
  if (s->session || s->trans)
    session_end (s, "server stopping");
  trans_delete (s->listener);
  s->listener = NULL;
  s->running = 0;
  memset (&arc, 0, sizeof arc);     /* no reconnecting to a stopped server */
  rdps_tls_forget ();
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
  if (s->waiting)
    {
      s->waiting->sck = -1;     /* the module closes the sockets */
      trans_delete (s->waiting);
    }
  if (s->in_login)
    login_close (&s->login);
  plat_set_clients (0);
  if (s->running)
    log_end ();                 /* server_start starts libxrdp's log again */
  {
    /* (the desktop having shut down outlives the server starting again) */
    int sd = s->shutdown_done;
    memset (s, 0, sizeof *s);
    s->shutdown_done = sd;
  }
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

/* A name for an RDP client's TS_UD_CS_CORE version. */
static void
rdp_version_text (unsigned v, char *t, size_t n)
{
  unsigned minor = v & 0xffff;
  if (v == 0)
    snprintf (t, n, "-");
  else if ((v >> 16) != 8)
    snprintf (t, n, "unknown (&%08X)", v);
  else if (minor == 1)
    snprintf (t, n, "RDP 4");
  else if (minor == 4)
    snprintf (t, n, "RDP 5 to 8.1");
  else if (minor >= 5 && minor <= 0x11)
    snprintf (t, n, "RDP 10.%u", minor - 5);
  else if (minor > 0x11)
    snprintf (t, n, "RDP 10.12 or later (&%08X)", v);
  else
    snprintf (t, n, "RDP (&%08X)", v);
}

/* The Status window's details of the connection. */
static void
status_details (server *s, server_status *st)
{
  struct xrdp_client_info *ci = s->session ? s->session->client_info : NULL;
  st->connected_s = s->was_up && !s->in_login && s->up_since
                    ? (plat_time_cs () - s->up_since) / 100 : 0;
  if (s->scr.width)
    {
      const char *c;
      switch (s->fmt)
        {
        case PIX_1_PAL: c = "2 colours"; break;
        case PIX_2_PAL: c = "4 colours"; break;
        case PIX_4_PAL: c = "16 colours"; break;
        case PIX_8_PAL: c = "256 colours"; break;
        case PIX_16_444_TBGR: case PIX_16_444_TRGB: c = "4096 colours"; break;
        case PIX_16_555_TBGR: case PIX_16_555_TRGB: c = "32 thousand colours"; break;
        case PIX_16_565_TBGR: case PIX_16_565_TRGB: c = "64 thousand colours"; break;
        case PIX_32_TBGR: case PIX_32_TRGB: c = "16 million colours"; break;
        default: c = "can't be shared"; break;
        }
      snprintf (st->screen, sizeof st->screen, "%d x %d, %s", s->scr.width, s->scr.height, c);
    }
  if (!ci || !s->trans)
    return;
  copy_text (st->hostname, sizeof st->hostname, ci->hostname);
  rdp_version_text ((unsigned) ci->rdp_client_version, st->protocol, sizeof st->protocol);
  if (s->trans->tls)
    snprintf (st->security, sizeof st->security, "%s, %s",
              s->trans->ssl_protocol ? s->trans->ssl_protocol : "TLS",
              s->trans->cipher_name ? s->trans->cipher_name : "?");
  else
    snprintf (st->security, sizeof st->security, "Standard RDP Security (RC4)");
  if (!s->was_up)
    return;
  if (s->ka_caps & 2)
    {
      if (s->rtt_ok)
        snprintf (st->keepalive, sizeof st->keepalive, "%snetwork checks: round trip %u cs",
                  s->ka_caps & 1 ? "heartbeats, " : "", s->last_rtt_cs);
      else
        snprintf (st->keepalive, sizeof st->keepalive, "%snetwork checks (no answer yet)",
                  s->ka_caps & 1 ? "heartbeats, " : "");
    }
  else
    snprintf (st->keepalive, sizeof st->keepalive, "%s",
              s->ka_caps & 1 ? "heartbeats" : "none (the client doesn't take it)");
  if (!s->send_sound && s->snd.stage == 0)
    snprintf (st->sound, sizeof st->sound, "off (Choices)");
  else if (s->sound_closed && s->snd.stage == 0)
    snprintf (st->sound, sizeof st->sound, "from the next client (Choices)");
  else if (s->in_login)
    snprintf (st->sound, sizeof st->sound, "after the login");
  else if (s->snd.stage == 3)
    snprintf (st->sound, sizeof st->sound, "to the client, %u Hz 16-bit stereo%s",
              s->snd.rate, s->snd.mute_local ? ", this computer muted" : "");
  else if (s->snd.stage > 0)
    snprintf (st->sound, sizeof st->sound, "starting");
  else
    snprintf (st->sound, sizeof st->sound, "none");
}

void
server_get_status (server_status *st)
{
  server *s = &srv;
  memset (st, 0, sizeof *st);
  st->listening = s->running;
  /* at the login screen: still connecting, as far as anyone can tell */
  st->connected = s->was_up && !s->in_login;
  st->connecting = s->trans != NULL && (!s->was_up || s->in_login);
  st->at_login = s->in_login;
  st->kbytes_sent = rdps_bytes_sent / 1024;
  status_details (s, st);
  copy_text (st->client, sizeof st->client, s->client_desc);
  copy_text (st->user, sizeof st->user, s->client_user);
  st->width = s->scr.width;
  st->height = s->scr.height;
  st->bpp = s->client_bpp;
  st->tiles_sent = s->tiles_sent;
  st->login_set = s->logins.count > 0;
}
