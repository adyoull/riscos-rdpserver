/* RDPSProbe: the RISC OS calls. See ro.h.

   Variable numbers: the mode variables (0-12) are from the classic PRM's
   table (Vol 1, OS_ReadModeVariable); ScreenStart 148 and DisplayStart 149
   from the 2026 PRM's "VDU Variables" list (p1388). OS_Word 21: PRM p549
   gives the reason codes; the block (+0 reason, +1 X, +3 Y, each a 2-byte
   little-endian OS-unit value) is the classic PRM's.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdint.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "ro.h"

enum
{
  V_MODEFLAGS = 0, V_NCOLOUR = 3, V_XEIG = 4, V_YEIG = 5,
  V_LINELENGTH = 6, V_SCREENSIZE = 7, V_LOG2BPP = 9,
  V_XWINDLIMIT = 11, V_YWINDLIMIT = 12,
  V_SCREENSTART = 148, V_DISPLAYSTART = 149
};

const char *probe_read_mode (probe_mode *m)
{
  static const int in[] =
    { V_MODEFLAGS, V_NCOLOUR, V_XEIG, V_YEIG, V_LINELENGTH, V_SCREENSIZE,
      V_LOG2BPP, V_XWINDLIMIT, V_YWINDLIMIT, V_SCREENSTART, V_DISPLAYSTART,
      -1 };
  int out[11];
  const _kernel_oserror *e;

  memset (out, 0, sizeof out);
  e = _swix (OS_ReadVduVariables, _INR (0, 1), in, out);
  if (e)
    return e->errmess;
  m->modeflags = out[0];
  m->ncolour = out[1];
  m->xeig = out[2];
  m->yeig = out[3];
  m->line_length = out[4];
  m->screen_size = out[5];
  m->log2bpp = out[6];
  m->width = out[7] + 1;
  m->height = out[8] + 1;
  m->screen_start = (uintptr_t) (unsigned) out[9];
  m->display_start = (uintptr_t) (unsigned) out[10];
  return NULL;
}

unsigned probe_time_cs (void)
{
  int t = 0;
  if (_swix (OS_ReadMonotonicTime, _OUT (0), &t))
    return 0;
  return (unsigned) t;
}

static void put16 (unsigned char *p, int v)
{
  p[0] = (unsigned char) (v & 0xff);
  p[1] = (unsigned char) ((v >> 8) & 0xff);
}

static int get16 (const unsigned char *p)
{
  return (int16_t) (uint16_t) (p[0] | (p[1] << 8));
}

const char *probe_set_mouse (int x, int y)
{
  unsigned char b[5];
  const _kernel_oserror *e;

  b[0] = 3;
  put16 (b + 1, x);
  put16 (b + 3, y);
  e = _swix (OS_Word, _INR (0, 1), 21, b);
  return e ? e->errmess : NULL;
}

static const char *read21 (int reason, int *x, int *y)
{
  unsigned char b[5];
  const _kernel_oserror *e;

  memset (b, 0, sizeof b);
  b[0] = (unsigned char) reason;
  e = _swix (OS_Word, _INR (0, 1), 21, b);
  if (e)
    return e->errmess;
  *x = get16 (b + 1);
  *y = get16 (b + 3);
  return NULL;
}

const char *probe_read_mouse (int *x, int *y)
{
  return read21 (4, x, y);
}

const char *probe_read_pointer (int *x, int *y)
{
  return read21 (6, x, y);
}
