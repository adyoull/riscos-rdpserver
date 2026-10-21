/* RDPServer: sending what changed on the screen: bitmaps, and fills for
   solid areas.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include "srv.h"


/* Sends the changed tiles. A whole screen takes a while to compress (the
   first frame, or everything changing at once), so every 2 cs the
   thread lets other programs run (srvthread.h): on RISC OS the desktop
   never stops for more than about that. */
#define SEND_SLICE_CS 2

void
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
      pix_convert_best (s->in_login ? PIX_32_TBGR : s->fmt, cap_pixel (&s->cap, x, y),
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
