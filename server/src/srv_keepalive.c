/* RDPServer: keep-alive: heartbeats, network checks, a connection waiting
   while the client is checked.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include "srv.h"


/* Heartbeats, network checks, and what they show (see KA_PERIOD_S).
   Returns -1 if the session ended. */
int
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
      if (s->rtt_seen == 0 || s->verbose)
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
void
waiting_step (server *s, unsigned now)
{
  struct trans *t = s->waiting;
  if (t == NULL)
    return;
  if (s->trans == NULL)
    {
      s->waiting = NULL;
      if (plat_memory_reserve (MEMORY_RESERVE) != 0)
        {
          memory_say ("refused the waiting connection");
          trans_delete (t);
          return;
        }
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
