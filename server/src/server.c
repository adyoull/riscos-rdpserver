/* RDPServer: the server loop, the libxrdp session and its input.

   The way a session is driven (the trans set-up, reading whole PDUs in
   xrdp_session_data_in, libxrdp_process_incoming then
   libxrdp_process_data) follows xrdp's xrdp/xrdp_process.c (xrdp 0.10.6,
   Apache-2.0, Copyright (C) Jay Sorg 2004-2014), which does the same for
   xrdp's own window manager.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include "srv.h"


volatile int server_stop;
volatile int server_prereset;
volatile int server_shutdown_seen;
int g_rdps_desktop_width, g_rdps_desktop_height;

server srv;

/* Messages: to stdout (the task window, Linux), or to the front end's
   hook (the Wimp version keeps the last few for its Status window and
   writes them to a log file). */
static void (*say_hook) (const char *line);

void
server_set_message_hook (void (*hook) (const char *line))
{
  say_hook = hook;
}

void
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
void
copy_text (char *to, size_t size, const char *from)
{
  size_t n = strlen (from);
  if (n >= size)
    n = size - 1;
  memcpy (to, from, n);
  to[n] = 0;
}

/* What the step is doing now, for finding what holds the machine up:
   a phase that held it for 10 cs or more (not counting the waits in it,
   srvthread.h) is logged, at most 5 times in 5 seconds with the detailed
   log, 5 times a server run without */
#define SLOW_CS 10
void
phase (server *s, const char *name)
{
  unsigned t = plat_time_cs (), y = rdps_yielded_cs;
  if (s->ph_name)
    {
      unsigned held = (t - s->ph_t) - (y - s->ph_y);
      if (held >= SLOW_CS && (int) held > 0 && s->slow_said < 5)
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
  s->scan_rows = tune (TUNE_SCAN_ROWS);
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
     every row. */
  s->sample = tune (TUNE_SAMPLE_ROWS);
  if (s->sample >= 0)
    {
      s->sample = sample_snap (s->sample);
      say ("RDPServer$SampleRows: reading %s", s->sample > 1 ? "1 row in" : "every row");
      if (s->sample > 1)
        say ("  %d each pass", s->sample);
    }
  else
    s->sample = o->sample_rows > 0 ? sample_snap (o->sample_rows) : 8;
  /* For comparing: RDPServer$Fills 0 sends solid areas as
     bitmaps; RDPServer$BulkCompress 0 doesn't MPPC-compress updates. */
  s->fills_var = tune (TUNE_FILLS);
  if (s->fills_var == 0)
    say ("RDPServer$Fills: solid areas sent as bitmaps, not fills");
  s->bulk_var = tune (TUNE_BULK_COMPRESS);
  if (s->bulk_var == 0)
    say ("RDPServer$BulkCompress: updates not bulk compressed (MPPC)");
  /* RDPServer$SoundRate 0: sound at the rate the computer plays at, not
     halved to 22 kHz or so */
  s->sound_full = tune (TUNE_SOUND_RATE) == 0;
  if (s->sound_full)
    say ("RDPServer$SoundRate 0: sound at the full rate");
  s->snd.chan = -1;
  s->opt = o;
  s->verbose = o->verbose != 0;
  s->mute_local = o->mute_local;
  s->require_tls = o->require_tls;
  s->send_sound = o->send_sound;
  inq_init (&s->input);
  /* For comparing: RDPServer$ClickTime, how long a click
     holds the button (cs, 2 to 50; 6); RDPServer$Clicks 0, buttons down
     and up as the client sends them. */
  {
    int ct = tune (TUNE_CLICK_TIME), cm = tune (TUNE_CLICKS);
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
    /* libxrdp's messages, at INFO always: a few dozen lines as each
       client connects (its security, its capabilities), little after
       (these stay with the detailed log off);
       to the console, or to a file (the Wimp version: a Wimp task
       mustn't print). */
    struct log_config *lc = log_config_init_for_console (LOG_LEVEL_INFO, NULL);
    if (lc)
      {
        if (o->log_file)
          {
            lc->enable_console = 0;
            lc->log_file = strdup (o->log_file);
            lc->log_level = LOG_LEVEL_INFO;
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
  neon_say ();
  screen_copy_choose (s);
  encode_speed_say ();
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

/* step() is a turn of the server, in phases; each says GO_ON for the
   next phase, or what step() returns (how soon it wants its next turn,
   in cs). */
#define GO_ON (-1)

/* A claim failed: the block kept back was given up so that
   libxrdp's small claims still succeed, but the session can't go on.
   Then the block kept back, again if it was given up (the handshake needs
   it too, before a session starts). Without it no connection is taken:
   accepting one claims memory in libxrdp's trans layer already. */
static int
step_memory (server *s, int *mem_ok)
{
  if (plat_memory_short ())
    {
      memory_say ("out of memory");
      if (s->trans)
        {
          session_end (s, "out of memory (see the line before)");
          return 0;
        }
    }
  *mem_ok = plat_memory_reserve (MEMORY_RESERVE) == 0;
  if (!*mem_ok && !s->mem_refusing)
    memory_say ("not taking connections until there's memory again");
  else if (*mem_ok && s->mem_refusing)
    say ("memory again: taking connections");
  s->mem_refusing = !*mem_ok;
  return GO_ON;
}

/* Waits for the network (at most max_wait_cs), takes a new connection,
   and reads what the client sent. */
static int
step_network (server *s, unsigned now, unsigned max_wait_cs, int mem_ok)
{
  fd_set rd, wr;
  struct timeval tv;
  int backlog, maxfd, tls_more;
  unsigned wait_cs;

  phase (s, "reading from the client");
  maxfd = (int) s->listener->sck;
  FD_ZERO (&rd);
  FD_ZERO (&wr);
  if (mem_ok)
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
  return GO_ON;
}

/* RISC OS is restarting: the page, then closing (srv_restart.c). */
static int
step_restart (server *s)
{
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
  return GO_ON;
}

/* The client's input to RISC OS. */
static int
step_input (server *s)
{
  inq_run (&s->input, plat_time_cs (), &s->sink);
  if (s->input.clicked)
    {
      /* Let the Wimp see the button down now, before its up (by ticker,
         INQ_CLICK_CS from now): no scanning or sending this time. */
      s->input.clicked = 0;
      return 0;
    }
  return GO_ON;
}

/* The session coming up, or going back to waiting for that. */
static int
step_session (server *s)
{
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
  return GO_ON;
}

/* Sound, keep-alive and what holds sending back: the login screen's time
   limit, the client minimised or reactivating. */
static int
step_keepalive (server *s, unsigned now)
{
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
  return GO_ON;
}

/* The login screen, in the RISC OS screen's place. *now is read again
   once it's gone. */
static int
step_login (server *s, unsigned *now)
{
  phase (s, "the login screen, or reading the screen");
  if (s->was_up && s->in_login)
    {
      if (s->login_due)
        {
          login_try (s);
          if (!s->trans)
            return 0;
        }
      if (s->in_login)
        {
          if (s->trans->wait_s == NULL && (int) ((*now) - s->next_scan) >= 0)
            {
              /* the page is compared only after it changed */
              if (s->login.changed)
                {
                  s->login.changed = 0;
                  cap_scan (&s->cap, (const unsigned char *) s->login.px,
                            (size_t) s->login.w * 4, s->login.h);
                }
              send_dirty (s);
              s->next_scan = *now + 5;
            }
          return 2;
        }
      *now = plat_time_cs ();
    }
  return GO_ON;
}

/* Reads the next band of the screen, and sends what changed. */
static int
step_scan (server *s, unsigned now)
{
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
             bank, keep the mode, rather than taking the sprite's mode for
             a mode change. */
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
         banks with OS_Byte 113; not a mode change): read the bank that's shown now. */
      s->scr.base = cur.base;
      /* A pass over the screen: its time, and a program may have changed
         the palette. A band never runs past the bottom, so every pass
         starts at row 0. (Waiting for row 0 with bands that don't divide
         the height would come round only every few passes.) */
      if (s->cap.next_row == 0)
        {
          if (s->pass_start)
            s->pass_cs = now - s->pass_start;
          s->pass_start = now;
          palette_refresh (s, redirected);
        }
      /* The rows due since the last band, so a pass takes the scan
         period however often turns come (a fixed number of rows a turn
         would make a pass's time depend on how often turns come); at most
         scan_rows if that's set, and not past the bottom. */
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
  return GO_ON;
}

/* How soon the next turn is wanted. */
static int
step_next (server *s)
{
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

int
step (unsigned max_wait_cs)
{
  server *s = &srv;
  int r, mem_ok;
  unsigned now = plat_time_cs ();

  if (!s->running)
    return 25;
  if ((r = step_memory (s, &mem_ok)) != GO_ON
      || (r = step_network (s, now, max_wait_cs, mem_ok)) != GO_ON
      || (r = step_restart (s)) != GO_ON
      || (r = step_input (s)) != GO_ON
      || (r = step_session (s)) != GO_ON)
    return r;
  now = plat_time_cs ();
  if ((r = step_keepalive (s, now)) != GO_ON
      || (r = step_login (s, &now)) != GO_ON
      || (r = step_scan (s, now)) != GO_ON)
    return r;
  return step_next (s);
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
      if (gap >= GAP_LOG_CS && s->verbose)
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
      if (s->verbose && s->trans)
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
      if (s->verbose)
        s->slow_said = 0;     /* the plain log: 5 a server run */
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
  arc_forget ();
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
  arc_forget ();     /* logins changed: password again */
  return 0;
}

/* Choices changed "only allow TLS connections": from the next client. */
void
server_set_require_tls (int on)
{
  srv.require_tls = on != 0;
}

/* Choices' "detailed log": every click, key, wheel and network check,
   and the figures every 5 s, from now. (libxrdp's own log keeps the level
   it started with: it says little after a client connects.) */
void
server_set_verbose (int on)
{
  server *s = &srv;
  if ((on != 0) == s->verbose)
    return;
  s->verbose = on != 0;
  s->snd.verbose = s->verbose;
  say (s->verbose ? "detailed log on" : "detailed log off");
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
  arc_forget ();     /* no reconnecting to a stopped server */
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
