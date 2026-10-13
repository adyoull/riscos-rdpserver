/* RDPSProbe: what an RDP server on RISC OS would have to work with.

   Measures how fast the screen can be read (the cost of every capture),
   compared with ordinary memory, how fast a capture loop can look for
   changes, and (with -p) whether the mouse can be moved the way a remote
   client's input would move it.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "probe.h"
#include "ro.h"
#include "measure.h"

#define TILE 64

static void rate_line (FILE *out, const char *what, probe_rate r, double frame_bytes)
{
  if (r.cs == 0 || r.passes == 0)
    {
      fprintf (out, "  %-34s too fast to time (%u passes)\n", what, r.passes);
      return;
    }
  fprintf (out, "  %-34s %8.1f MB/s  %7.1f ms a frame  (%u passes, %u cs)\n",
           what, r.mb_per_s, frame_bytes / (r.mb_per_s * 1e6) * 1000.0,
           r.passes, r.cs);
}

static int pointer_test (FILE *out, const probe_mode *m)
{
  int ox, oy, x, y, mx, my, px, py, ok;
  const char *e;
  unsigned t0, guard;

  e = probe_read_mouse (&ox, &oy);
  if (e)
    {
      fprintf (out, "  reading the mouse failed: %s\n", e);
      return 1;
    }
  /* Somewhere else on the screen: a quarter of the way in from the
     bottom left corner, in OS units. */
  x = (m->width << m->xeig) / 4;
  y = (m->height << m->yeig) / 4;
  if (x == ox && y == oy)
    x += 64;
  e = probe_set_mouse (x, y);
  if (e)
    {
      fprintf (out, "  OS_Word 21,3 (set mouse position) failed: %s\n", e);
      return 1;
    }
  e = probe_read_mouse (&mx, &my);
  /* The pointer follows the mouse at the next VSync: wait up to 10 cs. */
  t0 = probe_time_cs ();
  for (guard = 0; guard < 10000000u && probe_time_cs () - t0 < 10; guard++)
    ;
  if (!e)
    e = probe_read_pointer (&px, &py);
  probe_set_mouse (ox, oy);   /* put it back */
  if (e)
    {
      fprintf (out, "  reading back failed: %s\n", e);
      return 1;
    }
  ok = mx == x && my == y && px == x && py == y;
  fprintf (out, "  set the mouse to %d,%d: mouse reads %d,%d, pointer %d,%d: %s\n",
           x, y, mx, my, px, py, ok ? "PASS" : "FAIL");
  fprintf (out, "  (put back at %d,%d)\n", ox, oy);
  return ok ? 0 : 1;
}

/* Left Shift: low-level key number &4C (p1247), INKEY/internal 3 (p1241).
   Shift on its own types nothing, so pressing it can't do any harm. */
#define LL_LEFT_SHIFT 0x4C
#define IN_LEFT_SHIFT 3

static int wait_key (int want, int *down)
{
  unsigned t0 = probe_time_cs (), guard;
  const char *e = NULL;
  for (guard = 0; guard < 10000000u; guard++)
    {
      e = probe_key_down (IN_LEFT_SHIFT, down);
      if (e || *down == want || probe_time_cs () - t0 >= 10)
        break;
    }
  return e == NULL;
}

static int key_test (FILE *out)
{
  int before = 0, held = 0, after = 1, ok;
  const char *e;

  e = probe_key_down (IN_LEFT_SHIFT, &before);
  if (e)
    {
      fprintf (out, "  OS_Byte 121 failed: %s\n", e);
      return 1;
    }
  if (before)
    {
      fprintf (out, "  Left Shift is already down: let go of the keyboard and try again\n");
      return 1;
    }
  e = probe_keyv (LL_LEFT_SHIFT, 1);
  if (e)
    {
      fprintf (out, "  KeyV 2 (key pressed) through OS_CallAVector failed: %s\n", e);
      return 1;
    }
  wait_key (1, &held);
  e = probe_keyv (LL_LEFT_SHIFT, 0);   /* always let go */
  if (e)
    fprintf (out, "  KeyV 1 (key released) failed: %s: press and release Shift\n", e);
  wait_key (0, &after);
  ok = !e && held && !after;
  fprintf (out, "  Left Shift through KeyV: seen down %s, seen up again %s: %s\n",
           held ? "yes" : "no", after ? "no" : "yes", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}

int probe_run (FILE *out, const probe_options *o)
{
  probe_mode m;
  probe_frame f;
  const char *e;
  size_t row_bytes, frame_bytes;
  unsigned char *copy, *copy2, *mask, *rowbuf;
  probe_rate r;
  int changed = 0, bytes_pp, tiles, dirty, fails = 0;
  unsigned t0, t1, i, reps;

  fprintf (out, "RDPSProbe " PROBE_VERSION "\n");
  e = probe_read_mode (&m);
  if (e)
    {
      fprintf (out, "Can't read the screen mode: %s\n", e);
      return 1;
    }
  fprintf (out, "Screen: %d x %d, Log2BPP %d, NColour %d, ModeFlags &%X, "
           "eig %d,%d\n", m.width, m.height, m.log2bpp, m.ncolour,
           (unsigned) m.modeflags, m.xeig, m.yeig);
  fprintf (out, "  LineLength %d, ScreenSize %d, ScreenStart &%08lX, "
           "DisplayStart &%08lX\n", m.line_length, m.screen_size,
           (unsigned long) m.screen_start, (unsigned long) m.display_start);

  if (m.width <= 0 || m.height <= 0 || m.log2bpp < 0 || m.log2bpp > 5
      || m.display_start == 0)
    {
      fprintf (out, "That doesn't look like a screen this probe can read.\n");
      return 1;
    }
  row_bytes = ((size_t) m.width << m.log2bpp) / 8;
  if ((size_t) m.line_length < row_bytes
      || (m.screen_size > 0
          && (size_t) m.line_length * (m.height - 1) + row_bytes
             > (size_t) m.screen_size))
    {
      fprintf (out, "The mode's sizes don't add up; not reading the screen.\n");
      return 1;
    }
  if (m.log2bpp < 4)
    fprintf (out, "  (a palette mode: an RDP server would want 16 or 32 bpp)\n");

  frame_bytes = row_bytes * m.height;
  copy = malloc (frame_bytes);
  copy2 = malloc (frame_bytes);
  bytes_pp = m.log2bpp >= 3 ? 1 << (m.log2bpp - 3) : 1;
  tiles = ((m.width + TILE - 1) / TILE) * ((m.height + TILE - 1) / TILE);
  mask = malloc ((size_t) tiles);
  rowbuf = malloc (row_bytes);
  if (!copy || !copy2 || !mask || !rowbuf)
    {
      fprintf (out, "Not enough memory for two copies of the screen "
               "(%lu bytes each): give it a bigger WimpSlot.\n",
               (unsigned long) frame_bytes);
      free (copy);
      free (copy2);
      free (mask);
      free (rowbuf);
      return 1;
    }

  f.base = (const unsigned char *) m.display_start;
  f.rows = m.height;
  f.row_bytes = row_bytes;
  f.stride = (size_t) m.line_length;

  fprintf (out, "\nCopying (%lu bytes a frame, %u cs for each test):\n",
           (unsigned long) frame_bytes, o->min_cs);
  r = probe_bench_copy (copy, &f, probe_time_cs, o->min_cs, o->max_passes);
  rate_line (out, "screen to memory", r, (double) frame_bytes);

  {
    probe_frame g = { copy, m.height, row_bytes, row_bytes };
    r = probe_bench_copy (copy2, &g, probe_time_cs, o->min_cs, o->max_passes);
    rate_line (out, "memory to memory", r, (double) frame_bytes);
  }

  r = probe_bench_compare (copy, &f, probe_time_cs, o->min_cs, o->max_passes,
                           &changed);
  rate_line (out, "comparing, memcmp on the screen", r, (double) frame_bytes);
  fprintf (out, "  (%d of %d rows had changed by the last comparison)\n",
           changed, m.height);

  r = probe_bench_compare_words (copy, &f, probe_time_cs, o->min_cs,
                                 o->max_passes, &changed);
  rate_line (out, "comparing, word loads on the screen", r, (double) frame_bytes);

  /* The capture pass a server would use: memcpy each row out, compare in
     memory, keep the copy up to date. */
  r = probe_bench_bounce (copy2, &f, rowbuf, probe_time_cs, o->min_cs,
                          o->max_passes, &changed);
  rate_line (out, "row copied out, then compared", r, (double) frame_bytes);
  fprintf (out, "  (%d rows changed in its last pass)\n", changed);
  memcpy (copy, copy2, frame_bytes);

  /* Changes found in memory: copy2 is copy; change one tile, then time the
     tile scan an RDP server would run on two frames in memory. */
  memcpy (copy2, copy, frame_bytes);
  copy2[frame_bytes / 2] ^= 0xff;
  reps = 0;
  t0 = probe_time_cs ();
  t1 = t0;
  dirty = 0;
  for (i = 0; i < o->max_passes; i++)
    {
      dirty = probe_dirty_tiles (copy, copy2, m.width, m.height, bytes_pp,
                                 TILE, mask);
      reps++;
      t1 = probe_time_cs ();
      if (t1 - t0 >= o->min_cs)
        break;
    }
  if (t1 > t0)
    fprintf (out, "  %-34s %8.1f ms a frame (%d of %d tiles dirty)\n",
             "64x64 tile scan, memory", (t1 - t0) * 10.0 / reps, dirty, tiles);
  else
    fprintf (out, "  %-34s too fast to time (%d of %d tiles dirty)\n",
             "64x64 tile scan, memory", dirty, tiles);
  if (dirty != 1)
    {
      fprintf (out, "  FAIL: the scan should have found exactly one tile\n");
      fails++;
    }

  if (o->keys)
    {
      fprintf (out, "\nPressing a key (KeyV):\n");
      fails += key_test (out);
    }

  if (o->pointer)
    {
      fprintf (out, "\nMoving the mouse (OS_Word 21):\n");
      fails += pointer_test (out, &m);
    }

  free (copy);
  free (copy2);
  free (mask);
  free (rowbuf);
  fprintf (out, "\nDone.\n");
  return fails ? 1 : 0;
}
