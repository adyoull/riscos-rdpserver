/* RDPServer: finding what changed on the screen. See capture.h.
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdlib.h>
#include <string.h>
#include "capture.h"

int
cap_init (capture *c, int width, int height, int bits)
{
  memset (c, 0, sizeof *c);
  if (width <= 0 || height <= 0 || bits <= 0 || bits > 32 || (bits & (bits - 1)))
    return -1;
  c->width = width;
  c->height = height;
  c->bits = bits;
  c->sample = 1;
  c->row_bytes = ((size_t) width * bits + 7) / 8;
  c->tiles_x = (width + CAP_TILE - 1) / CAP_TILE;
  c->tiles_y = (height + CAP_TILE - 1) / CAP_TILE;
  c->frame = calloc ((size_t) height, c->row_bytes);
  c->rowbuf = malloc (c->row_bytes);
  c->rowchg = malloc ((size_t) c->tiles_x);
  c->dirty = malloc ((size_t) c->tiles_x * c->tiles_y);
  c->box = malloc ((size_t) c->tiles_x * c->tiles_y * 4 * sizeof *c->box);
  if (!c->frame || !c->rowbuf || !c->rowchg || !c->dirty || !c->box)
    {
      cap_free (c);
      return -1;
    }
  cap_mark_all (c);
  return 0;
}

void
cap_free (capture *c)
{
  free (c->frame);
  free (c->rowbuf);
  free (c->rowchg);
  free (c->dirty);
  free (c->box);
  c->frame = c->rowbuf = c->rowchg = c->dirty = NULL;
  c->box = NULL;
}

/* The first and last pixel (from the start of a tile's piece of a row)
   in byte n of it. Under 8 bits a pixel, a byte holds several, and the
   rectangles sent start and end on whole bytes. */
static int
first_pixel (const capture *c, size_t n)
{
  return (int) (n * 8 / (size_t) c->bits);
}

static int
last_pixel (const capture *c, size_t n)
{
  if (c->bits >= 8)
    return (int) (n / (size_t) (c->bits / 8));
  return (int) ((n + 1) * 8 / (size_t) c->bits) - 1;
}

/* Adds a rectangle (screen pixels, inclusive) to tile t's changed box,
   widened to whole bytes and clipped to the screen. */
static void
grow (capture *c, int t, int x0, int y0, int x1, int y1)
{
  unsigned short *b = c->box + 4 * t;
  if (c->bits < 8)
    {
      int per_byte = 8 / c->bits;
      x0 &= ~(per_byte - 1);
      x1 |= per_byte - 1;
    }
  if (x1 > c->width - 1)
    x1 = c->width - 1;
  if (!c->dirty[t])
    {
      b[0] = (unsigned short) x0; b[1] = (unsigned short) y0;
      b[2] = (unsigned short) x1; b[3] = (unsigned short) y1;
      return;
    }
  if (x0 < b[0]) b[0] = (unsigned short) x0;
  if (y0 < b[1]) b[1] = (unsigned short) y0;
  if (x1 > b[2]) b[2] = (unsigned short) x1;
  if (y1 > b[3]) b[3] = (unsigned short) y1;
}

/* --- copying out of the screen ---------------------------------------- */

int cap_copy_way = CAP_COPY_LDM8;

#ifdef RDPS_MODULE
void rdps_copy_ldm8 (void *dst, const void *src, size_t n);    /* scrcopy.s */
void rdps_copy_words (void *dst, const void *src, size_t n);
#else
static void rdps_copy_ldm8 (void *dst, const void *src, size_t n) { memcpy (dst, src, n); }
static void rdps_copy_words (void *dst, const void *src, size_t n) { memcpy (dst, src, n); }
#endif

void
cap_copy_with (int way, void *dst, const void *src, size_t n)
{
  if (way == CAP_COPY_WORDS)
    rdps_copy_words (dst, src, n);
  else
    rdps_copy_ldm8 (dst, src, n);
}

void
cap_copy (void *dst, const void *src, size_t n)
{
  cap_copy_with (cap_copy_way, dst, src, n);
}

/* --- finding changes ---------------------------------------------------- */

/* One tile's piece of row y, `now` (len bytes, just read), against the
   last frame: if they differ, the frame is updated and the tile's box
   grown. Returns 1 if that made the tile dirty, 0 if not. *changed is set
   if anything differed. */
static int
piece_check (capture *c, int y, int tx, const unsigned char *now, size_t len,
             int *changed)
{
  size_t piece = (size_t) CAP_TILE * c->bits / 8, off = (size_t) tx * piece;
  unsigned char *last = c->frame + (size_t) y * c->row_bytes + off;
  unsigned char *drow = c->dirty + (size_t) (y / CAP_TILE) * c->tiles_x;
  size_t lo = 0, hi = len - 1;
  int t = (int) (drow - c->dirty) + tx, px = tx * CAP_TILE;

  if (memcmp (now, last, len) == 0)
    return 0;
  *changed = 1;
  /* which pixels: the first and last bytes that differ */
  while (now[lo] == last[lo])
    lo++;
  while (now[hi] == last[hi])
    hi--;
  memcpy (last, now, len);
  grow (c, t, px + first_pixel (c, lo), y, px + last_pixel (c, hi), y);
  if (drow[tx])
    return 0;
  drow[tx] = 1;
  return 1;
}

/* the length of tile column tx's piece of a row */
static size_t
piece_len (const capture *c, int tx)
{
  size_t piece = (size_t) CAP_TILE * c->bits / 8, off = (size_t) tx * piece;
  return off + piece <= c->row_bytes ? piece : c->row_bytes - off;
}

/* Reads screen row y whole and checks every tile's piece of it. If
   `changed` isn't NULL, changed[tx] says whether tile column tx's piece
   differed. */
static int
row_check (capture *c, const unsigned char *base, size_t stride, int y,
           unsigned char *changed)
{
  size_t piece = (size_t) CAP_TILE * c->bits / 8;
  int tx, newly = 0;

  cap_copy (c->rowbuf, base + (size_t) y * stride, c->row_bytes);
  c->bytes_read += c->row_bytes;
  for (tx = 0; tx < c->tiles_x; tx++)
    {
      int ch = 0;
      newly += piece_check (c, y, tx, c->rowbuf + (size_t) tx * piece,
                            piece_len (c, tx), &ch);
      if (changed)
        changed[tx] = (unsigned char) ch;
    }
  return newly;
}

int
cap_scan (capture *c, const unsigned char *base, size_t stride, int rows)
{
  int n, newly = 0;

  if (rows > c->height)
    rows = c->height;
  for (n = 0; n < rows; n++)
    {
      int y = c->next_row;
      newly += row_check (c, base, stride, y, NULL);
      c->next_row = y + 1 == c->height ? 0 : y + 1;
    }
  return newly;
}

/* The order the rows of each group of `sample` are read in, pass by pass:
   the bits of the pass number reversed (for 8: 0 4 2 6 1 5 3 7), so the
   rows read so far are spread out, not bunched. */
static int
sample_row (int sample, int phase)
{
  int r = 0, b;
  for (b = 1; b < sample; b <<= 1)
    {
      r <<= 1;
      if (phase & b)
        r |= 1;
    }
  return r;
}

int
cap_scan_sampled (capture *c, const unsigned char *base, size_t stride, int rows)
{
  int n, newly = 0, sample = c->sample;
  size_t piece = (size_t) CAP_TILE * c->bits / 8;

  if (sample <= 1 || (sample & (sample - 1)) || sample > CAP_TILE)
    return cap_scan (c, base, stride, rows);
  if (rows > c->height)
    rows = c->height;
  for (n = 0; n < rows; n++)
    {
      int y = c->next_row;
      if ((y & (sample - 1)) == sample_row (sample, c->phase))
        {
          int tx;
          newly += row_check (c, base, stride, y, c->rowchg);
          /* Where the row changed, read the rest of that tile at once:
             the change may cover rows this pass doesn't read. */
          for (tx = 0; tx < c->tiles_x; tx++)
            if (c->rowchg[tx])
              {
                int y0 = y / CAP_TILE * CAP_TILE, y1 = y0 + CAP_TILE, yy, ch = 0;
                size_t len = piece_len (c, tx), off = (size_t) tx * piece;
                if (y1 > c->height)
                  y1 = c->height;
                for (yy = y0; yy < y1; yy++)
                  if (yy != y)
                    {
                      cap_copy (c->rowbuf + off, base + (size_t) yy * stride + off, len);
                      c->bytes_read += len;
                      newly += piece_check (c, yy, tx, c->rowbuf + off, len, &ch);
                    }
              }
        }
      if (y + 1 == c->height)
        {
          c->next_row = 0;
          c->phase = (c->phase + 1) % sample;
        }
      else
        c->next_row = y + 1;
    }
  return newly;
}

void
cap_mark (capture *c, int x, int y, int w, int h)
{
  int tx0, ty0, tx1, ty1, tx, ty;

  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > c->width) w = c->width - x;
  if (y + h > c->height) h = c->height - y;
  if (w <= 0 || h <= 0)
    return;
  tx0 = x / CAP_TILE;
  ty0 = y / CAP_TILE;
  tx1 = (x + w - 1) / CAP_TILE;
  ty1 = (y + h - 1) / CAP_TILE;
  for (ty = ty0; ty <= ty1; ty++)
    for (tx = tx0; tx <= tx1; tx++)
      {
        int t = ty * c->tiles_x + tx;
        int bx0 = tx * CAP_TILE, by0 = ty * CAP_TILE;
        int bx1 = bx0 + CAP_TILE - 1, by1 = by0 + CAP_TILE - 1;
        grow (c, t, x > bx0 ? x : bx0, y > by0 ? y : by0,
              x + w - 1 < bx1 ? x + w - 1 : bx1, y + h - 1 < by1 ? y + h - 1 : by1);
        c->dirty[t] = 1;
      }
}

void
cap_mark_all (capture *c)
{
  memset (c->dirty, 0, (size_t) c->tiles_x * c->tiles_y);
  cap_mark (c, 0, 0, c->width, c->height);
}

int
cap_next_dirty (capture *c, int *index, int *x, int *y, int *w, int *h)
{
  int n = c->tiles_x * c->tiles_y, i;

  for (i = *index; i < n; i++)
    if (c->dirty[i])
      {
        const unsigned short *b = c->box + 4 * i;
        c->dirty[i] = 0;
        *x = b[0];
        *y = b[1];
        *w = b[2] - b[0] + 1;
        *h = b[3] - b[1] + 1;
        *index = i + 1;
        return 1;
      }
  *index = n;
  return 0;
}

const unsigned char *
cap_pixel (const capture *c, int x, int y)
{
  return c->frame + (size_t) y * c->row_bytes + (size_t) x * c->bits / 8;
}
