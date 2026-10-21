/* RDPServer: the Status window's details.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include "srv.h"


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
              s->snd.out_rate, s->snd.mute_local ? ", this computer muted" : "");
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
