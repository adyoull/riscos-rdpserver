/* RDPServer: measurements at start-up: reading the screen, encoding, NEON.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include "srv.h"

/* At start-up: how fast this computer encodes the screen as it is, at 32
   and 16 bpp (encspeed.c), for comparing with what the 5-second figures
   show. About 0.25 s. */
void
encode_speed_say (void)
{
  rdps_screen scr;
  static const int depths[2] = { 32, 16 };
  int i, fast_ok[2] = { 0, 0 }, neon_ok[2] = { 0, 0 };
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
      /* the other ways of converting, each used
         only if it made exactly the same pixels at both depths */
      if (r.fast_tps)
        {
          fast_ok[i] = r.fast_same;
          neon_ok[i] = r.neon_tps && r.neon_same;
          if (r.neon_tps)
            say ("converting at %d bpp, other ways: a simpler loop %u ms%s, NEON %u ms%s",
                 depths[i], rdps_enc_ms (r.screen_tiles, r.fast_tps),
                 r.fast_same ? "" : " (DIFFERENT pixels)",
                 rdps_enc_ms (r.screen_tiles, r.neon_tps),
                 r.neon_same ? " (the same pixels)" : " (DIFFERENT pixels)");
          else
            say ("converting at %d bpp, other ways: a simpler loop %u ms%s", depths[i],
                 rdps_enc_ms (r.screen_tiles, r.fast_tps),
                 r.fast_same ? " (the same pixels)" : " (DIFFERENT pixels)");
        }
    }
  pix_use_fast = fast_ok[0] && fast_ok[1];
  pix_use_neon = neon_ok[0] && neon_ok[1] && pix_use_fast;   /* (the loop does
                                                                 NEON's leftovers) */
  if (pix_use_neon || pix_use_fast)
    say ("converting by %s (32 bpp screens; others the general way)",
         pix_use_neon ? "NEON" : "the simpler loop");
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

/* NEON: whether the module could make its own VFP context (not at all
   with RDPServer$NEON 0), and what switching to it and back costs with a
   little NEON work in between (8 pixels converted). */
void
neon_say (void)
{
  static uint32_t in[8], out[8];
  unsigned t0, t, n = 0;
  pix_use_neon = pix_use_fast = 0;     /* until shown to make the same pixels */
  if (tune (TUNE_NEON) == 0)
    {
      plat_neon_stop ();        /* one made by an earlier server run, too:
                                   every plat_neon_enter fails from now */
      say ("NEON: not used (RDPServer$NEON 0)");
      return;
    }
  if (!plat_neon_start ())
    {
      say ("NEON: not here (no VFPSupport, or no NEON)");
      return;
    }
  t0 = plat_time_cs ();
  do
    {
      int i;
      for (i = 0; i < 100; i++)
        if (plat_neon_enter () == 0)
          {
            plat_neon_convert (0, 32, out, in, 8);
            plat_neon_leave ();
            n++;
          }
      t = plat_time_cs ();
    }
  while ((int) (t - t0) < 5 && n < 1000000);
  if (n)
    say ("NEON: its own VFP context, a switch there and back %u.%u us",
         (t - t0) * 10000 / n, (t - t0) * 100000 / n % 10);
  else
    say ("NEON: the module's VFP context couldn't be switched to");
}

/* At start-up: time copying out of the screen by single words and by
   LDM of eight words, say so, and use the quicker. *Set
   RDPServer$ScreenCopy 1 (words) or 2 (LDM 8)
   to choose instead. */
void
screen_copy_choose (server *s)
{
  rdps_screen scr;
  int forced = tune (TUNE_SCREEN_COPY);
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
      unsigned n = 0;
      cap_copy_way = l >= w ? CAP_COPY_LDM8 : CAP_COPY_WORDS;
      /* NEON: timed as cap_scan uses it,
         the context switched to around the run */
      if (plat_neon_enter () == 0)
        {
          n = screen_copy_speed (&scr, row, row_bytes, CAP_COPY_NEON, 10);
          plat_neon_leave ();
        }
      if (n > (l >= w ? l : w))
        cap_copy_way = CAP_COPY_NEON;
      free (row);
      if (n)
        say ("screen read: single words %u MB/s, LDM 8 %u MB/s, NEON %u MB/s",
             w / 1024, l / 1024, n / 1024);
      else
        say ("screen read: single words %u MB/s, LDM 8 %u MB/s", w / 1024, l / 1024);
    }
  if (forced == CAP_COPY_WORDS || forced == CAP_COPY_LDM8
      || (forced == CAP_COPY_NEON && tune (TUNE_NEON) != 0
          && plat_neon_start ()))
    cap_copy_way = forced;
  say ("screen read by %s%s", cap_copy_way == CAP_COPY_NEON ? "NEON"
       : cap_copy_way == CAP_COPY_LDM8 ? "LDM 8" : "single words",
       forced == cap_copy_way ? " (RDPServer$ScreenCopy)" : "");
}
