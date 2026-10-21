/* RDPServer: RISC OS restarting: the "RISC OS is restarting" page for the
   client.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include "srv.h"



/* RISC OS is about to reset (Service_PreReset, from the module): the
   client gets a page saying so in the screen's place, and nothing more;
   then the module closes the connection as if the network had gone, so
   the client reconnects by itself once the computer is back (if
   RDPServer starts with it), showing the page meanwhile. */

/* The click (or Return) held back for the restarting page, now. */
void
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

void
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
