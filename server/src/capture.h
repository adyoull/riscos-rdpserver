/* RDPServer: finding what changed on the screen.

   The screen is only ever read with memcpy, a row at a time, into ordinary
   memory: on a Pi 4 small loads from the screen are slow (RDPSProbe 0.1:
   memcmp on the screen 42 MB/s, memcpy 680 MB/s; 0.2: this way, 20.5 ms a
   1080p frame). Each row is compared with the last frame in 64-pixel
   pieces, so each changed piece marks one 64x64 tile; the copy is kept up
   to date, and tiles are sent from it. Since 0.2-test15 each dirty tile
   also keeps the rectangle within it that changed (the rows, and the
   first and last pixel that differ in them), and only that is sent: a
   typed letter or the caret is a few pixels, not a 64x64 bitmap.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_CAPTURE_H
#define RDPS_CAPTURE_H

#include <stddef.h>
#include "platform.h"

#define CAP_TILE 64

typedef struct
{
  int width, height, bits;      /* bits per screen pixel: 1, 2, 4, 8, 16 or 32 */
  size_t row_bytes;
  unsigned char *frame;         /* the last frame, packed rows */
  unsigned char *rowbuf;        /* one row, copied out of the screen */
  unsigned char *rowchg;        /* per tile column: changed in that row */
  int tiles_x, tiles_y;
  unsigned char *dirty;         /* one byte per tile */
  unsigned short *box;          /* per tile: x0, y0, x1, y1 (inclusive,
                                   screen pixels) of what changed */
  int next_row;                 /* where the next scan starts */
  int sample;                   /* cap_scan_sampled: 1 row in this many */
  int phase;                    /* which of them this pass (an index into
                                   the order, see capture.c) */
  unsigned long long bytes_read; /* read from the screen, for the figures */
} capture;

/* How rows are copied out of the screen (module: src/scrcopy.s; the host:
   memcpy). CAP_COPY_LDM8 unless set otherwise. */
#define CAP_COPY_WORDS 1
#define CAP_COPY_LDM8 2
#define CAP_COPY_NEON 3    /* NEON (0.2-test30): cap_scan switches to the
                             module's VFP context around a scan; outside one,
                             as LDM8 */
extern int cap_copy_way;
void cap_copy (void *dst, const void *src, size_t n);
void cap_copy_with (int way, void *dst, const void *src, size_t n);

/* Sets up for a screen of this size and depth (bytes a pixel). Marks every
   tile dirty: the first frame is sent whole. Returns 0, or -1 if out of
   memory. */
int cap_init (capture *c, int width, int height, int bits);
void cap_free (capture *c);

/* Scans up to `rows` rows of the screen, carrying on where the last scan
   stopped and wrapping at the bottom. base/stride: the screen. Returns
   how many tiles became dirty. */
int cap_scan (capture *c, const unsigned char *base, size_t stride, int rows);

/* As cap_scan, but of the rows in the band reads only 1 in c->sample
   (1, 2, 4, 8 or 16; set by cap_init to 1), a different set each pass,
   so that every row is read once in `sample` passes. Where a row read has
   changed in a tile, the whole tile is read at once. A change at least
   `sample` rows tall is found in the pass it happens in; a smaller one
   within `sample` passes. Returns how many tiles became dirty. */
int cap_scan_sampled (capture *c, const unsigned char *base, size_t stride, int rows);

/* Marks the tiles under a rectangle dirty (a client asked for a redraw). */
void cap_mark (capture *c, int x, int y, int w, int h);
void cap_mark_all (capture *c);

/* The next dirty tile at or after *index (row by row), cleared as it's
   returned; the rectangle within it that changed in *x,*y,*w,*h. Returns
   0 when there are none. */
int cap_next_dirty (capture *c, int *index, int *x, int *y, int *w, int *h);

/* A pointer to pixel (x, y) in the last frame. */
const unsigned char *cap_pixel (const capture *c, int x, int y);

#endif
