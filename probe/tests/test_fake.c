/* RDPSProbe's RISC OS part on Linux, against the fake RISC OS: a fake
   screen in memory, a fake mouse, and a clock that ticks once a call.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "fake.h"
#include "swis.h"
#include "../src/ro.h"
#include "../src/probe.h"

static unsigned char *screen;
static int scr_w = 200, scr_h = 100, scr_l2 = 5, scr_line = 1024, scr_size;
static int mouse_x = 10, mouse_y = 20, ptr_x = 10, ptr_y = 20;
static int pointer_lags;   /* 1: the pointer never follows the mouse */
static unsigned clock_cs;

static const _kernel_oserror *vdu (int swi, int r[10])
{
  const int *in = (const int *) (intptr_t) r[0];
  int *out = (int *) (intptr_t) r[1];
  (void) swi;
  for (; *in != -1; in++, out++)
    switch (*in)
      {
      case 0: *out = 0; break;
      case 3: *out = -1; break;
      case 4: case 5: *out = 1; break;
      case 6: *out = scr_line; break;
      case 7: *out = scr_size; break;
      case 9: *out = scr_l2; break;
      case 11: *out = scr_w - 1; break;
      case 12: *out = scr_h - 1; break;
      case 148: case 149: *out = (int) (intptr_t) screen; break;
      default:
        fprintf (stderr, "unexpected VDU variable %d\n", *in);
        abort ();
      }
  return NULL;
}

static int g16 (const unsigned char *p) { return (int16_t) (p[0] | (p[1] << 8)); }
static void p16 (unsigned char *p, int v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }

static const _kernel_oserror *word (int swi, int r[10])
{
  unsigned char *b = (unsigned char *) (intptr_t) r[1];
  (void) swi;
  if (r[0] != 21)
    abort ();
  switch (b[0])
    {
    case 3:
      mouse_x = g16 (b + 1);
      mouse_y = g16 (b + 3);
      if (!pointer_lags)
        {
          ptr_x = mouse_x;
          ptr_y = mouse_y;
        }
      break;
    case 4: p16 (b + 1, mouse_x); p16 (b + 3, mouse_y); break;
    case 6: p16 (b + 1, ptr_x); p16 (b + 3, ptr_y); break;
    default: abort ();
    }
  return NULL;
}

static const _kernel_oserror *ticks (int swi, int r[10])
{
  (void) swi;
  r[0] = (int) clock_cs++;
  return NULL;
}

static void setup (void)
{
  fake_reset ();
  fake_swi (OS_ReadVduVariables, "OS_ReadVduVariables", vdu);
  fake_swi (OS_Word, "OS_Word", word);
  fake_swi (OS_ReadMonotonicTime, "OS_ReadMonotonicTime", ticks);
  mouse_x = ptr_x = 10;
  mouse_y = ptr_y = 20;
  scr_size = scr_line * scr_h;
  free (screen);
  screen = malloc ((size_t) scr_size);
  memset (screen, 0x55, (size_t) scr_size);
}

static char report[8192];

static int run (int pointer)
{
  probe_options o = { 5, 50, pointer };
  FILE *f = fmemopen (report, sizeof report, "w");
  int rc = probe_run (f, &o);
  fclose (f);
  return rc;
}

int main (void)
{
  probe_mode m;
  int x, y;

  setup ();
  CHECK (probe_read_mode (&m) == NULL, "the mode is read");
  CHECK (m.width == 200 && m.height == 100, "size from the window limits + 1");
  CHECK (m.log2bpp == 5 && m.line_length == 1024 && m.ncolour == -1, "format and line length");
  CHECK (m.display_start == (uintptr_t) screen, "DisplayStart is the screen");

  CHECK (probe_set_mouse (-3, 300) == NULL, "set the mouse");
  CHECK (mouse_x == -3 && mouse_y == 300, "negative and big values go as 2-byte values");
  CHECK (probe_read_mouse (&x, &y) == NULL && x == -3 && y == 300, "and read back");

  CHECK (run (0) == 0, "a 32 bpp screen probes cleanly");
  CHECK (strstr (report, "Screen: 200 x 100, Log2BPP 5") != NULL, "the report names the mode");
  CHECK (strstr (report, "screen to memory") && strstr (report, "MB/s"), "the copy rate is reported");
  CHECK (strstr (report, "(1 of 8 tiles dirty)") != NULL, "the tile scan finds the one change");
  CHECK (strstr (report, "Moving the mouse") == NULL, "no mouse test without -p");
  CHECK (mouse_x == -3, "and the mouse is left alone");

  /* The screen changes under the comparison: rows are counted. */
  setup ();
  CHECK (run (1) == 0, "the mouse test passes when the pointer follows");
  CHECK (strstr (report, "PASS") != NULL, "it says so");
  CHECK (mouse_x == 10 && mouse_y == 20, "and the mouse is put back");

  setup ();
  pointer_lags = 1;
  CHECK (run (1) != 0, "a pointer that doesn't follow fails");
  CHECK (strstr (report, "FAIL") != NULL, "and the report says so");
  pointer_lags = 0;

  /* Sizes that don't add up: the screen isn't read past its end. */
  setup ();
  scr_size = scr_line * (scr_h - 1);
  CHECK (run (0) != 0 && strstr (report, "don't add up") != NULL, "a screen too small isn't read");
  setup ();
  scr_line = 100;   /* less than 200 pixels x 4 bytes */
  CHECK (run (0) != 0, "a line length shorter than a row is refused");
  scr_line = 1024;

  /* 16 bpp: half the bytes a row. */
  setup ();
  scr_l2 = 4;
  CHECK (run (0) == 0 && strstr (report, "(40000 bytes a frame") != NULL, "16 bpp rows are 400 bytes");
  scr_l2 = 3;
  setup ();
  CHECK (run (0) == 0 && strstr (report, "a palette mode") != NULL, "8 bpp is measured, with a note");
  scr_l2 = 5;

  free (screen);
  return fake_done ("RDPSProbe (fake RISC OS)");
}
