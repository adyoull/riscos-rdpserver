/* RDPServer: how fast this computer encodes the screen, measured once at
   start-up on the screen as it is (the middle 64 rows), stage by stage:
   converting to the client's depth (pix_convert), compressing each
   64x64 tile (libxrdp's interleaved RLE, or planar at 32 bpp) and bulk
   compressing what that makes (MPPC, when the client asks for it). Each
   stage runs over the same tiles for a few centiseconds; the figures are
   scaled to a whole screen.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdlib.h>
#include <string.h>
#include "libxrdp.h"
#include "platform.h"
#include "pixels.h"
#include "capture.h"
#include "encspeed.h"

#define TILE 64
#define MAX_TILES 64

/* runs fn over the tiles again and again for cs centiseconds: tiles a
   second */
typedef int (*stage_fn) (void *ctx, int t);

static unsigned
rate (stage_fn fn, void *ctx, int ntiles, unsigned cs)
{
  unsigned t0 = plat_time_cs (), t;
  unsigned long done = 0;
  int passes = 0;
  do
    {
      int i;
      for (i = 0; i < ntiles; i++)
        fn (ctx, i);
      done += (unsigned long) ntiles;
      passes++;
      t = plat_time_cs ();
    }
  while ((int) (t - t0) < (int) cs && passes < 10000);
  return t == t0 ? 0 : (unsigned) (done * 100 / (t - t0));
}

typedef struct
{
  pix_format fmt;
  const unsigned char *band;    /* the 64 rows, packed */
  size_t row_bytes;
  int bits, cbpp;
  unsigned char *conv;          /* ntiles converted tiles */
  struct stream *out[MAX_TILES];/* ntiles compressed tiles */
  struct stream *tmp;
  struct xrdp_mppc_enc *mppc;
  unsigned long rle_bytes, mppc_bytes;
  int count_bytes;
} enc_ctx;

static int
do_convert (void *v, int t)
{
  enc_ctx *c = v;
  pix_convert (c->fmt, c->band + (size_t) t * TILE * c->bits / 8, c->row_bytes,
               TILE, TILE, c->cbpp, c->conv + (size_t) t * TILE * TILE * 4);
  return 0;
}

static int
do_compress (void *v, int t)
{
  enc_ctx *c = v;
  struct stream *s = c->out[t];
  char *tile = (char *) c->conv + (size_t) t * TILE * TILE * 4;
  init_stream (s, 16384);
  init_stream (c->tmp, 65536);
  if (c->cbpp > 24)
    xrdp_bitmap32_compress (tile, TILE, TILE, s, 32, 16000, TILE - 1, c->tmp, 0, 0x10);
  else
    xrdp_bitmap_compress (tile, TILE, TILE, s, c->cbpp, 16000, TILE - 1, c->tmp, 0);
  s_mark_end (s);
  if (c->count_bytes)
    c->rle_bytes += (unsigned long) (s->end - s->data);
  return 0;
}

static int
do_mppc (void *v, int t)
{
  enc_ctx *c = v;
  struct stream *s = c->out[t];
  int n = (int) (s->end - s->data);
  if (compress_rdp (c->mppc, (tui8 *) s->data, n) && c->count_bytes)
    c->mppc_bytes += (c->mppc->flags & 0x20) ? (unsigned long) c->mppc->bytes_in_opb
                                             : (unsigned long) n;
  else if (c->count_bytes)
    c->mppc_bytes += (unsigned long) n;
  return 0;
}

int
rdps_encode_speed (const rdps_screen *scr, pix_format fmt, int cbpp, unsigned cs,
                   rdps_enc_speed *r)
{
  enc_ctx c;
  int ntiles, i, y0, ok = -1;
  unsigned long screen_tiles;
  size_t row_bytes;

  memset (r, 0, sizeof *r);
  memset (&c, 0, sizeof c);
  if (scr->width < TILE || scr->height < TILE || pix_is_palette (fmt))
    return -1;
  c.fmt = fmt;
  c.cbpp = cbpp;
  c.bits = 1 << scr->log2bpp;
  row_bytes = ((size_t) scr->width * (size_t) c.bits) / 8;
  c.row_bytes = row_bytes;
  ntiles = scr->width / TILE;
  if (ntiles > MAX_TILES)
    ntiles = MAX_TILES;
  c.band = malloc (row_bytes * TILE);
  c.conv = malloc ((size_t) ntiles * TILE * TILE * 4);
  make_stream (c.tmp);
  c.mppc = mppc_enc_new (PROTO_RDP_50);
  for (i = 0; i < ntiles; i++)
    make_stream (c.out[i]);
  if (!c.band || !c.conv || !c.tmp || !c.mppc)
    goto done;
  for (i = 0; i < ntiles; i++)
    if (!c.out[i])
      goto done;
  /* the middle 64 rows, out of the screen as the scan reads them */
  y0 = scr->height / 2 - TILE / 2;
  for (i = 0; i < TILE; i++)
    cap_copy ((unsigned char *) c.band + row_bytes * (size_t) i,
              scr->base + (size_t) (y0 + i) * scr->stride, row_bytes);

  /* one counted pass of each, then the timed ones */
  for (i = 0; i < ntiles; i++)
    do_convert (&c, i);
  c.count_bytes = 1;
  for (i = 0; i < ntiles; i++)
    do_compress (&c, i);
  for (i = 0; i < ntiles; i++)
    do_mppc (&c, i);
  c.count_bytes = 0;
  r->convert_tps = rate (do_convert, &c, ntiles, cs);
  r->compress_tps = rate (do_compress, &c, ntiles, cs);
  /* (the timed MPPC passes compress what the last compress pass made) */
  r->mppc_tps = rate (do_mppc, &c, ntiles, cs);

  screen_tiles = (unsigned long) ((scr->width + TILE - 1) / TILE)
                 * (unsigned long) ((scr->height + TILE - 1) / TILE);
  r->screen_tiles = (unsigned) screen_tiles;
  r->tiles = ntiles;
  r->rle_kb = (unsigned) ((unsigned long long) c.rle_bytes * screen_tiles / (unsigned) ntiles / 1024);
  r->mppc_kb = (unsigned) ((unsigned long long) c.mppc_bytes * screen_tiles / (unsigned) ntiles / 1024);
  ok = 0;

done:
  for (i = 0; i < ntiles && i < MAX_TILES; i++)
    if (c.out[i])
      free_stream (c.out[i]);
  if (c.tmp)
    free_stream (c.tmp);
  if (c.mppc)
    mppc_enc_free (c.mppc);
  free ((void *) c.band);
  free (c.conv);
  return ok;
}

unsigned
rdps_enc_ms (unsigned screen_tiles, unsigned tps)
{
  return tps ? (unsigned) ((unsigned long) screen_tiles * 1000 / tps) : 0;
}
