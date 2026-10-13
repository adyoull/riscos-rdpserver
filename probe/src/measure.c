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

static int row_differs_words (const unsigned char *a, const unsigned char *b,
                              size_t n)
{
  const uint32_t *p = (const uint32_t *) (const void *) a;
  const uint32_t *q = (const uint32_t *) (const void *) b;
  size_t words = n / 4, i = 0;

  for (; i + 8 <= words; i += 8)
    if ((p[i] ^ q[i]) | (p[i + 1] ^ q[i + 1]) | (p[i + 2] ^ q[i + 2])
        | (p[i + 3] ^ q[i + 3]) | (p[i + 4] ^ q[i + 4]) | (p[i + 5] ^ q[i + 5])
        | (p[i + 6] ^ q[i + 6]) | (p[i + 7] ^ q[i + 7]))
      return 1;
  for (; i < words; i++)
    if (p[i] != q[i])
      return 1;
  for (i = words * 4; i < n; i++)
    if (a[i] != b[i])
      return 1;
  return 0;
}

probe_rate probe_bench_compare_words (const unsigned char *copy, const probe_frame *f,
                                      probe_clock_fn clk, unsigned min_cs,
                                      unsigned max_passes, int *changed_rows)
{
  unsigned start, passes = 0, now;
  int changed = 0;

  /* Word loads need every row 4-byte aligned, in the frame and in the
     packed copy: otherwise (8 bpp, an odd width) it's memcmp's job. */
  if ((((uintptr_t) f->base | (uintptr_t) copy | f->stride | f->row_bytes) & 3) != 0)
    return probe_bench_compare (copy, f, clk, min_cs, max_passes, changed_rows);

  start = now = clk ();
  while (passes < max_passes)
    {
      const unsigned char *s = f->base;
      const unsigned char *c = copy;
      int y;
      changed = 0;
      for (y = 0; y < f->rows; y++)
        {
          changed += row_differs_words (s, c, f->row_bytes);
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

probe_rate probe_bench_bounce (unsigned char *copy, const probe_frame *f,
                               unsigned char *rowbuf, probe_clock_fn clk,
                               unsigned min_cs, unsigned max_passes,
                               int *changed_rows)
{
  unsigned start = clk (), passes = 0, now = start;
  int changed = 0;

  while (passes < max_passes)
    {
      const unsigned char *s = f->base;
      unsigned char *c = copy;
      int y;
      changed = 0;
      for (y = 0; y < f->rows; y++)
        {
          memcpy (rowbuf, s, f->row_bytes);
          if (memcmp (rowbuf, c, f->row_bytes) != 0)
            {
              memcpy (c, rowbuf, f->row_bytes);
              changed++;
            }
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
