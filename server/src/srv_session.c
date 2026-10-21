/* RDPServer: a session: connecting, logging in, coming up, ending, auto-
   reconnect, the screen mode changing.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include "srv.h"

static void updates_say (server *s);


/* --- the session ---------------------------------------------------------- */

/* RDPServer$SendBuffer (KB, 16 to 1024): the client's socket send buffer
   to ask for instead of 64 KB, for trying others. */
int
sndbuf_from_var (void)
{
  int k = tune (TUNE_SEND_BUFFER);
  return k >= 16 && k <= 1024 ? k * 1024 : 0;
}

void
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
void
server_arc_connection_ended (void)
{
  if (arc.valid && arc.ended_cs == 0)
    arc.ended_cs = plat_time_cs () | 1;
}
static int arc_next_id = 1;

/* Forget the cookie: no client may reconnect with it. */
void
arc_forget (void)
{
  memset (&arc, 0, sizeof arc);
}

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
void
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
      if (s->verbose)
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
void
session_go (server *s, const char *user)
{
  struct xrdp_client_info *ci = s->session->client_info;
  copy_text (s->client_user, sizeof s->client_user, user);
  arc_send (s, ci);
  phase (s, "starting the sound");
  if (s->send_sound && !s->sound_closed)
    sound_start (&s->snd, s->session, s->mute_local, s->verbose, s->sound_full, say);
  else if (libxrdp_get_channel_id (s->session, "rdpsnd") >= 0)
    say ("sound: not sent (Choices: \"Send this computer's sound to the client\" is off)");
  s->last_activity = plat_time_cs ();
  say ("client up: %d x %d, %d bpp, user \"%s\"", s->scr.width, s->scr.height,
       s->client_bpp, user);
  memory_say (NULL);
  s->up_since = plat_time_cs ();
  s->pointer_due = 1;
  plat_set_clients (1);
}


/* The login screen's user name and password, when it asks (from step:
   ending the session or resizing the client isn't done in libxrdp's
   callback). */
void
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

int
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
  if (s->verbose)
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
  /* a block kept back for running out */
  if (plat_memory_reserve (MEMORY_RESERVE) != 0)
    {
      memory_say ("out of memory before the session started");
      return -1;
    }
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
      memory_say (NULL);
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

/* Memory: how much the module's heap has, of its most, and
   whether RISC OS refused it more (no free memory) */
void
memory_say (const char *what)
{
  unsigned used, max;
  int refused;
  plat_memory_info (&used, &max, &refused);
  if (!max)
    return;                     /* (Linux) */
  if (!what)
    say ("memory: %u MB of RDPServer's most %u in use", (used + 512) / 1024, max / 1024);
  else if (refused)
    say ("%s: RDPServer's memory is %u MB (most %u), and RISC OS had no more to "
         "give it: free some memory, or use a screen mode with fewer pixels",
         what, (used + 512) / 1024, max / 1024);
  else
    say ("%s: RDPServer's memory is %u MB, its most (a screen mode with fewer "
         "pixels needs less)", what, (used + 512) / 1024);
}

/* Solid areas go as OpaqueRect orders ([MS-RDPEGDI] 2.2.2.2.1.1.2.5), a
   few bytes, instead of a compressed bitmap, if the client takes them
   (orderSupport[TS_NEG_PATBLT_INDEX], which covers OpaqueRect).
   At 32 bpp, only to RDP 10 clients: the colour is red, green, blue from
   the lowest byte up at 24 and 32 bpp (as xrdp sends it, and FreeRDP and
   Windows take it), but rdesktop (and so RISC OS's RDPClient) takes it
   the other way round at 32 bpp, and says it's RDP 5, as Windows 7 and 8
   do: those get bitmaps at 32 bpp, which are right everywhere. */
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

int
is_term (void)
{
  server *s = &srv;
  if (server_stop || rdps_thread_ending ())
    return 1;
  return s->handshake_deadline != 0
         && (int) (plat_time_cs () - s->handshake_deadline) >= 0;
}

void
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

void
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

int
conn_in (struct trans *listener, struct trans *t)
{
  server *s = (server *) listener->callback_data;
  if ((s->refuse_until && (int) (plat_time_cs () - s->refuse_until) < 0)
      || s->restarting || s->restart_due)
    {
      trans_delete (t);
      return 0;
    }
  /* Without the block kept back for running out (it was given up and
     can't be had again: memory is short), a connection would meet
     libxrdp's unchecked claims with nothing to fall back on */
  if (s->trans == NULL && plat_memory_reserve (MEMORY_RESERVE) != 0)
    {
      char desc[64];
      g_sck_get_peer_description ((int) t->sck, desc, sizeof desc);
      memory_say ("refused a connection");
      say ("refused %s: out of memory", desc);
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

int
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
