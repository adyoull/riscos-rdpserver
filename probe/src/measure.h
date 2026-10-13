/* RDPSProbe: the portable measurements (no SWIs), so they can be tested
   under QEMU with RISC OS's alignment rules.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef PROBE_MEASURE_H
#define PROBE_MEASURE_H

#include <stddef.h>
#include <stdint.h>

typedef unsigned (*probe_clock_fn) (void);   /* centiseconds */

/* A frame: rows of row_bytes, each stride bytes apart. */
typedef struct
{
  const unsigned char *base;
  int rows;
  size_t row_bytes;
  size_t stride;
} probe_frame;

typedef struct
{
  unsigned passes;      /* whole frames done */
  unsigned cs;          /* time taken */
  double mb_per_s;      /* MB (10^6 bytes) a second; 0 if no time passed */
} probe_rate;

/* Copies the frame into dst (rows packed, row_bytes apart) again and again
   until at least min_cs centiseconds have passed or max_passes are done. */
probe_rate probe_bench_copy (unsigned char *dst, const probe_frame *f,
                             probe_clock_fn clk, unsigned min_cs,
                             unsigned max_passes);

/* Compares the frame with a packed copy row by row, as a capture loop
   looking for changes would. Same stopping rule. *changed_rows is the
   number of rows that differed in the last pass. */
probe_rate probe_bench_compare (const unsigned char *copy, const probe_frame *f,
                                probe_clock_fn clk, unsigned min_cs,
                                unsigned max_passes, int *changed_rows);

/* The same comparison with 32-bit loads, eight a loop, straight from the
   frame. Unless every row of both is 4-byte aligned (8 bpp, odd widths),
   it falls back to probe_bench_compare. */
probe_rate probe_bench_compare_words (const unsigned char *copy, const probe_frame *f,
                                      probe_clock_fn clk, unsigned min_cs,
                                      unsigned max_passes, int *changed_rows);

/* A capture pass as a server would run it: each row copied with memcpy
   into rowbuf (row_bytes, in ordinary cached memory), compared there with
   the packed copy, and the copy updated where it differs. Reading the
   screen only with memcpy suits a screen mapped uncached, where memcpy's
   multi-word loads are far faster than small ones. */
probe_rate probe_bench_bounce (unsigned char *copy, const probe_frame *f,
                               unsigned char *rowbuf, probe_clock_fn clk,
                               unsigned min_cs, unsigned max_passes,
                               int *changed_rows);

/* Marks which tile x tile squares differ between two packed frames of
   width x height pixels (bytes_pp bytes each). mask gets one byte per
   tile, row by row (tiles_x * tiles_y bytes). Returns the number of
   tiles that differ. */
int probe_dirty_tiles (const unsigned char *a, const unsigned char *b,
                       int width, int height, int bytes_pp, int tile,
                       unsigned char *mask);

#endif
