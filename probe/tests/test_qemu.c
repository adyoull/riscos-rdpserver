/* RDPSProbe's portable part as ARM code under qemu-arm-aligntrap: the
   measurements with RISC OS's alignment rules, on odd sizes and offsets.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/measure.h"

static unsigned now;
static unsigned clk (void) { return now++; }

static int fails;
#define CHECK(c, w) do { if (!(c)) { fails++; fprintf (stderr, "FAIL %d: %s\n", __LINE__, w); } } while (0)

int main (void)
{
  enum { W = 131, H = 67, BPP = 2 };   /* odd sizes: partial tiles */
  size_t row = W * BPP, stride = row + 6;
  unsigned char *scr = malloc (stride * H + 3);
  unsigned char *a = malloc (row * H), *b = malloc (row * H);
  unsigned char mask[3 * 2];
  probe_frame f;
  probe_rate r;
  int y, changed = -1, n;

  for (y = 0; y < (int) (stride * H + 3); y++)
    scr[y] = (unsigned char) (y * 7);
  /* An odd start address and stride, as a screen bank could never be,
     to show memcpy/memcmp cope with any alignment. */
  f.base = scr + 1;
  f.rows = H;
  f.row_bytes = row;
  f.stride = stride;

  r = probe_bench_copy (a, &f, clk, 3, 1000);
  CHECK (r.passes == 3 && r.cs == 3, "stops once the time is up");
  for (y = 0; y < H; y++)
    CHECK (memcmp (a + y * row, scr + 1 + y * stride, row) == 0, "rows copied packed");

  r = probe_bench_compare (a, &f, clk, 1000, 4, &changed);
  CHECK (r.passes == 4, "stops at max passes");
  CHECK (changed == 0, "an unchanged frame has no changed rows");
  scr[1 + 5 * stride + 3] ^= 1;
  scr[1 + 9 * stride] ^= 1;
  probe_bench_compare (a, &f, clk, 1, 1, &changed);
  CHECK (changed == 2, "two changed rows are counted");

  /* Word compare: needs aligned rows, so a frame of its own (malloc'd,
     stride a multiple of 4). Row 7 differs in its last byte, past the
     unrolled part. */
  {
    size_t wrow = 132 * 4, wstride = wrow + 8;
    unsigned char *w = malloc (wstride * H), *wc = malloc (wrow * H), *rb = malloc (wrow);
    probe_frame g = { w, H, wrow, wstride };
    for (y = 0; y < (int) (wstride * H); y++)
      w[y] = (unsigned char) (y * 13);
    probe_bench_copy (wc, &g, clk, 1, 1);
    probe_bench_compare_words (wc, &g, clk, 1, 1, &changed);
    CHECK (changed == 0, "words: unchanged");
    w[7 * wstride + wrow - 1] ^= 1;
    w[9 * wstride + 5] ^= 1;
    probe_bench_compare_words (wc, &g, clk, 1, 1, &changed);
    CHECK (changed == 2, "words: a last byte and an early byte both found");
    /* The bounce pass finds the same rows and brings the copy up to date. */
    probe_bench_bounce (wc, &g, rb, clk, 1, 1, &changed);
    CHECK (changed == 2, "bounce: two rows changed");
    probe_bench_bounce (wc, &g, rb, clk, 1, 1, &changed);
    CHECK (changed == 0, "bounce: the copy was updated");
    for (y = 0; y < H; y++)
      CHECK (memcmp (wc + y * wrow, w + y * wstride, wrow) == 0, "bounce: copy matches");
    free (w); free (wc); free (rb);
  }
  /* The odd frame (start +1, stride not a multiple of 4) falls back to
     memcmp under the alignment trap instead of aborting. */
  probe_bench_compare_words (a, &f, clk, 1, 1, &changed);
  CHECK (changed == 2, "words on an unaligned frame: the memcmp fallback");

  memcpy (b, a, row * H);
  n = probe_dirty_tiles (a, b, W, H, BPP, 64, mask);
  CHECK (n == 0, "no tiles differ");
  b[(66 * W + 130) * BPP] ^= 1;               /* bottom right pixel */
  b[(0 * W + 64) * BPP + 1] ^= 1;             /* top row, second tile */
  n = probe_dirty_tiles (a, b, W, H, BPP, 64, mask);
  CHECK (n == 2, "two tiles differ");
  CHECK (mask[1] == 1 && mask[5] == 1 && mask[0] == 0 && mask[4] == 0, "the right two");

  free (scr); free (a); free (b);
  printf ("RDPSProbe (QEMU): %s\n", fails ? "FAILED" : "all passed");
  return fails != 0;
}
