/* RDPSProbe: the RISC OS calls (src/ro.c). Everything that calls a SWI is
   here, so tests/test_fake.c can run it on Linux against the fake RISC OS.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef PROBE_RO_H
#define PROBE_RO_H

#include <stddef.h>
#include <stdint.h>

/* The current screen mode, from OS_ReadVduVariables. */
typedef struct
{
  int width, height;          /* pixels: XWindLimit+1, YWindLimit+1 */
  int log2bpp;                /* 3 = 8 bpp, 4 = 16 bpp, 5 = 32 bpp */
  int ncolour, modeflags;     /* together they give the pixel format */
  int xeig, yeig;             /* OS units per pixel = 1 << eig */
  int line_length;            /* bytes from one row to the next */
  int screen_size;            /* bytes in one screen bank */
  uintptr_t screen_start;     /* where the VDU draws */
  uintptr_t display_start;    /* what the display shows */
} probe_mode;

/* Fills *m. Returns NULL, or the error's text. */
const char *probe_read_mode (probe_mode *m);

/* OS_ReadMonotonicTime, centiseconds. */
unsigned probe_time_cs (void);

/* The pointer and mouse, in OS units (OS_Word 21). */
const char *probe_set_mouse (int x, int y);          /* reason 3 */
const char *probe_read_mouse (int *x, int *y);       /* reason 4, unbuffered */
const char *probe_read_pointer (int *x, int *y);     /* reason 6 */

/* Keys, as a remote keyboard would press them: KeyV 2 (pressed) or 1
   (released) through OS_CallAVector, with a low-level internal key number
   (PRM p1244, p1247). */
const char *probe_keyv (int low_level_key, int pressed);

/* OS_Byte 121 for one key (internal key number, the INKEY table, PRM
   p1241): sets *down to 1 if the kernel sees it held down. */
const char *probe_key_down (int internal_key, int *down);

#endif
