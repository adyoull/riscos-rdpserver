/* RDPServer: how long a whole screen takes to encode, stage by stage,
   and how many bytes it makes: what send_dirty does for each 64x64 tile
   (pix_convert, then libxrdp's RLE or planar compressor, then MPPC bulk
   compression of the update). Not part of the program; for measuring.

     bench_rle FILE.rgb WIDTH HEIGHT CLIENT_BPP [REPEATS]

   FILE.rgb: raw 8-bit R,G,B (ImageMagick "rgb:"), made into the screen's
   32 bpp TBGR. Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "arch.h"
#include "parse.h"
#include "libxrdp.h"
#include "pixels.h"
#include "bench_enc.h"

static double now_ms (void)
{
  struct timespec t;
  clock_gettime (CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

int main (int argc, char **argv)
{
  int w, h, cbpp, reps = 3, r, x, y, mode;
  unsigned char *rgb, *scr, *tile;
  FILE *f;
  struct stream *s, *tmp;
  struct xrdp_mppc_enc *mppc;
  static const char *names[] = { "libxrdp (now)", "solid tiles as fills", "fast path", "" };

  if (argc < 5)
    return 2;
  w = atoi (argv[2]);
  h = atoi (argv[3]);
  cbpp = atoi (argv[4]);
  if (argc > 5)
    reps = atoi (argv[5]);
  rgb = malloc ((size_t) w * h * 3);
  scr = malloc ((size_t) w * h * 4);
  tile = malloc (64 * 64 * 4);
  f = fopen (argv[1], "rb");
  if (!f || fread (rgb, 3, (size_t) w * h, f) != (size_t) w * h)
    return 1;
  fclose (f);
  for (y = 0; y < w * h; y++)
    {
      scr[4 * y] = rgb[3 * y];
      scr[4 * y + 1] = rgb[3 * y + 1];
      scr[4 * y + 2] = rgb[3 * y + 2];
      scr[4 * y + 3] = 0;
    }
  make_stream (s);
  init_stream (s, 16384 * 4);
  make_stream (tmp);
  init_stream (tmp, 65536);
  mppc = mppc_enc_new (PROTO_RDP_50);

  for (mode = 0; names[mode][0]; mode++)
    {
      double t_conv = 0, t_rle = 0, t_mppc = 0, t0;
      long bytes = 0, mbytes = 0, tiles = 0, solid = 0;
      for (r = 0; r < reps; r++)
        for (y = 0; y < h; y += 64)
          for (x = 0; x < w; x += 64)
            {
              int tw = x + 64 <= w ? 64 : w - x, th = y + 64 <= h ? 64 : h - y, n;
              int e = (4 - tw) & 3;
              const unsigned char *src = scr + ((size_t) y * w + x) * 4;
              t0 = now_ms ();
              if (mode >= 1 && bench_tile_solid (src, (size_t) w * 4, tw, th))
                {
                  /* an opaque rectangle order: about 15 bytes */
                  t_conv += now_ms () - t0;
                  bytes += 15;
                  mbytes += 15;
                  solid++;
                  tiles++;
                  continue;
                }
              if (mode == 2)
                bench_convert_fast (src, (size_t) w * 4, tw, th, cbpp, tile);
              else
                pix_convert (PIX_32_TBGR, src, (size_t) w * 4, tw, th, cbpp, tile);
              t_conv += now_ms () - t0;
              t0 = now_ms ();
              init_stream (s, 0);
              if (cbpp > 24)
                n = xrdp_bitmap32_compress ((char *) tile, tw + e, th, s, 32, 16000, th - 1, tmp, e, 0x10);
              else
                n = xrdp_bitmap_compress ((char *) tile, tw + e, th, s, cbpp, 16000, th - 1, tmp, e);
              t_rle += now_ms () - t0;
              (void) n;
              bytes += (long) (s->p - s->data) + 18;
              t0 = now_ms ();
              if (compress_rdp (mppc, (tui8 *) s->data, (int) (s->p - s->data)))
                mbytes += (mppc->flags & 0x20 /* PACKET_COMPRESSED */) ? mppc->bytes_in_opb + 18
                          : (long) (s->p - s->data) + 18;
              else
                mbytes += (long) (s->p - s->data) + 18;
              t_mppc += now_ms () - t0;
              tiles++;
            }
      printf ("%-22s %2d bpp: convert %6.1f ms, RLE %6.1f ms, MPPC %6.1f ms a screen; "
              "%5ld KB after RLE, %5ld KB after MPPC; %ld of %ld tiles solid\n",
              names[mode], cbpp, t_conv / reps, t_rle / reps, t_mppc / reps,
              bytes / reps / 1024, mbytes / reps / 1024, solid / reps, tiles / reps);
    }
  return 0;
}
