/* RDPServer: finding what changed on the screen.

   The screen is only ever read with memcpy, a row at a time, into ordinary
   memory: on a Pi 4 small loads from the screen are slow (RDPSProbe 0.1:
   memcmp on the screen 42 MB/s, memcpy 680 MB/s; 0.2: this way, 20.5 ms a
   1080p frame). Each row is compared with the last frame in 64-pixel
   pieces, so each changed piece marks one 64x64 tile; the copy is kept up
   to date, and tiles are sent from it.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_CAPTURE_H
#define RDPS_CAPTURE_H

#include <stddef.h>
#include "platform.h"

#define CAP_TILE 64

typedef struct
{
  int width, height, bpp;       /* bpp: bytes per screen pixel */
  size_t row_bytes;
  unsigned char *frame;         /* the last frame, packed rows */
  unsigned char *rowbuf;        /* one row, copied out of the screen */
  int tiles_x, tiles_y;
  unsigned char *dirty;         /* one byte per tile */
  int next_row;                 /* where the next scan starts */
} capture;

/* Sets up for a screen of this size and depth (bytes a pixel). Marks every
   tile dirty: the first frame is sent whole. Returns 0, or -1 if out of
   memory. */
int cap_init (capture *c, int width, int height, int bpp);
void cap_free (capture *c);

/* Scans up to `rows` rows of the screen, carrying on where the last scan
   stopped and wrapping at the bottom. base/stride: the screen. Returns
   how many tiles became dirty. */
int cap_scan (capture *c, const unsigned char *base, size_t stride, int rows);

/* Marks the tiles under a rectangle dirty (a client asked for a redraw). */
void cap_mark (capture *c, int x, int y, int w, int h);
void cap_mark_all (capture *c);

/* The next dirty tile at or after *index (row by row), cleared as it's
   returned; its rectangle in *x,*y,*w,*h. Returns 0 when there are none. */
int cap_next_dirty (capture *c, int *index, int *x, int *y, int *w, int *h);

/* A pointer to pixel (x, y) in the last frame. */
const unsigned char *cap_pixel (const capture *c, int x, int y);

#endif
