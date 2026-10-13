/* RDPSProbe: the portable measurements. See measure.h.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <string.h>
#include "measure.h"

static probe_rate finish (unsigned passes, unsigned cs, double bytes)
{
  probe_rate r;
  r.passes = passes;
  r.cs = cs;
  r.mb_per_s = cs ? bytes * passes / (cs / 100.0) / 1e6 : 0.0;
  return r;
}

probe_rate probe_bench_copy (unsigned char *dst, const probe_frame *f,
                             probe_clock_fn clk, unsigned min_cs,
                             unsigned max_passes)
{
  unsigned start = clk (), passes = 0, now = start;

  while (passes < max_passes)
    {
      const unsigned char *s = f->base;
      unsigned char *d = dst;
      int y;
      for (y = 0; y < f->rows; y++)
        {
          memcpy (d, s, f->row_bytes);
          s += f->stride;
          d += f->row_bytes;
        }
      passes++;
      now = clk ();
      if (now - start >= min_cs)
        break;
    }
  return finish (passes, now - start, (double) f->row_bytes * f->rows);
}

probe_rate probe_bench_compare (const unsigned char *copy, const probe_frame *f,
                                probe_clock_fn clk, unsigned min_cs,
                                unsigned max_passes, int *changed_rows)
{
  unsigned start = clk (), passes = 0, now = start;
  int changed = 0;

  while (passes < max_passes)
    {
      const unsigned char *s = f->base;
      const unsigned char *c = copy;
      int y;
      changed = 0;
      for (y = 0; y < f->rows; y++)
        {
          if (memcmp (s, c, f->row_bytes) != 0)
            changed++;
          s += f->stride;
          c += f->row_bytes;
        }
      passes++;
      now = clk ();
      if (now - start >= min_cs)
        break;
    }
  if (changed_rows)
    *changed_rows = changed;
  return finish (passes, now - start, (double) f->row_bytes * f->rows);
}

int probe_dirty_tiles (const unsigned char *a, const unsigned char *b,
                       int width, int height, int bytes_pp, int tile,
                       unsigned char *mask)
{
  int tiles_x = (width + tile - 1) / tile;
  int tiles_y = (height + tile - 1) / tile;
  size_t row_bytes = (size_t) width * bytes_pp;
  int tx, ty, dirty = 0;

  for (ty = 0; ty < tiles_y; ty++)
    {
      int y0 = ty * tile;
      int y1 = y0 + tile < height ? y0 + tile : height;
      for (tx = 0; tx < tiles_x; tx++)
        {
          int x0 = tx * tile;
          int x1 = x0 + tile < width ? x0 + tile : width;
          size_t off = (size_t) x0 * bytes_pp;
          size_t len = (size_t) (x1 - x0) * bytes_pp;
          int y, d = 0;
          for (y = y0; y < y1 && !d; y++)
            d = memcmp (a + y * row_bytes + off, b + y * row_bytes + off, len) != 0;
          mask[ty * tiles_x + tx] = (unsigned char) d;
          dirty += d;
        }
    }
  return dirty;
}
