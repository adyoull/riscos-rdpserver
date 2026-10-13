/* RDPServer: finding what changed on the screen. See capture.h.
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdlib.h>
#include <string.h>
#include "capture.h"

int
cap_init (capture *c, int width, int height, int bpp)
{
  memset (c, 0, sizeof *c);
  if (width <= 0 || height <= 0 || bpp <= 0)
    return -1;
  c->width = width;
  c->height = height;
  c->bpp = bpp;
  c->row_bytes = (size_t) width * bpp;
  c->tiles_x = (width + CAP_TILE - 1) / CAP_TILE;
  c->tiles_y = (height + CAP_TILE - 1) / CAP_TILE;
  c->frame = calloc ((size_t) height, c->row_bytes);
  c->rowbuf = malloc (c->row_bytes);
  c->dirty = malloc ((size_t) c->tiles_x * c->tiles_y);
  if (!c->frame || !c->rowbuf || !c->dirty)
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
  free (c->dirty);
  c->frame = c->rowbuf = c->dirty = NULL;
}

int
cap_scan (capture *c, const unsigned char *base, size_t stride, int rows)
{
  int n, newly = 0;
  size_t piece = (size_t) CAP_TILE * c->bpp;

  if (rows > c->height)
    rows = c->height;
  for (n = 0; n < rows; n++)
    {
      int y = c->next_row, tx;
      unsigned char *last = c->frame + (size_t) y * c->row_bytes;
      unsigned char *drow = c->dirty + (size_t) (y / CAP_TILE) * c->tiles_x;

      memcpy (c->rowbuf, base + (size_t) y * stride, c->row_bytes);
      for (tx = 0; tx < c->tiles_x; tx++)
        {
          size_t off = (size_t) tx * piece;
          size_t len = off + piece <= c->row_bytes ? piece : c->row_bytes - off;
          if (memcmp (c->rowbuf + off, last + off, len) != 0)
            {
              memcpy (last + off, c->rowbuf + off, len);
              if (!drow[tx])
                {
                  drow[tx] = 1;
                  newly++;
                }
            }
        }
      c->next_row = y + 1 == c->height ? 0 : y + 1;
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
      c->dirty[ty * c->tiles_x + tx] = 1;
}

void
cap_mark_all (capture *c)
{
  memset (c->dirty, 1, (size_t) c->tiles_x * c->tiles_y);
}

int
cap_next_dirty (capture *c, int *index, int *x, int *y, int *w, int *h)
{
  int n = c->tiles_x * c->tiles_y, i;

  for (i = *index; i < n; i++)
    if (c->dirty[i])
      {
        int tx = i % c->tiles_x, ty = i / c->tiles_x;
        c->dirty[i] = 0;
        *x = tx * CAP_TILE;
        *y = ty * CAP_TILE;
        *w = *x + CAP_TILE <= c->width ? CAP_TILE : c->width - *x;
        *h = *y + CAP_TILE <= c->height ? CAP_TILE : c->height - *y;
        *index = i + 1;
        return 1;
      }
  *index = n;
  return 0;
}

const unsigned char *
cap_pixel (const capture *c, int x, int y)
{
  return c->frame + (size_t) y * c->row_bytes + (size_t) x * c->bpp;
}
