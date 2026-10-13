/* RDPServer: the RISC OS side.

   Screen: OS_ReadVduVariables (mode variables 0, 3, 4, 5, 6, 9, 11, 12
   from the classic PRM's table; DisplayStart 149, PRM p1388). Mouse:
   OS_Word 21,3 (reason codes PRM p549; block layout from the classic PRM:
   +1 X, +3 Y, 2-byte OS units). Keys and buttons: KeyV 1/2 (PRM p1244)
   through OS_CallAVector (p290), KeyV = vector 19 (p1343). All three were
   checked on a Pi 4 with RDPSProbe 0.2. Wheel: PointerV 9 (p1313),
   PointerV = vector 38 (&26, p1343). RDPServer$Clients: OS_SetVarVal.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "platform.h"

#define KeyV 19
#define PointerV 38

const char *
plat_screen (rdps_screen *s)
{
  static const int in[] = { 0, 3, 4, 5, 6, 9, 11, 12, 149, -1 };
  int out[9];
  const _kernel_oserror *e;

  memset (out, 0, sizeof out);
  e = _swix (OS_ReadVduVariables, _INR (0, 1), in, out);
  if (e)
    return e->errmess;
  s->modeflags = (unsigned) out[0];
  s->ncolour = out[1];
  s->xeig = out[2];
  s->yeig = out[3];
  s->stride = (size_t) out[4];
  s->log2bpp = out[5];
  s->width = out[6] + 1;
  s->height = out[7] + 1;
  s->base = (const unsigned char *) (uintptr_t) (unsigned) out[8];
  return NULL;
}

void
plat_mouse_to (const rdps_screen *s, int x, int y)
{
  unsigned char b[5];
  int ox = x << s->xeig;
  int oy = (s->height - 1 - y) << s->yeig;   /* RISC OS: y up from the bottom */

  b[0] = 3;
  b[1] = (unsigned char) (ox & 0xff);
  b[2] = (unsigned char) ((ox >> 8) & 0xff);
  b[3] = (unsigned char) (oy & 0xff);
  b[4] = (unsigned char) ((oy >> 8) & 0xff);
  _swix (OS_Word, _INR (0, 1), 21, b);
}

void
plat_key (int low_level_key, int down)
{
  _swix (OS_CallAVector, _INR (0, 1) | _IN (9), down ? 2 : 1, low_level_key, KeyV);
}

/* Clicks (see clickup.s): the routines are copied into an RMA block, with
   a { key, pending } pair for each mouse button after them. OS_Module 6/7
   (claim/free RMA), OS_SynchroniseCodeAreas 1 (new code), OS_CallAfter
   (p289), OS_RemoveTickerEvent (p468). */
extern const unsigned char rdps_clickup_ticker[], rdps_clickup_end[];

static int *clickup;                    /* the RMA block (word-aligned) */
static int clickup_failed;

#define CODE_BYTES ((size_t) (rdps_clickup_end - rdps_clickup_ticker))

static volatile int *
click_slot (int ll)
{
  return clickup + (CODE_BYTES + 3) / 4 + 2 * (ll - 0x70);
}

static int
clickup_init (void)
{
  size_t words = (CODE_BYTES + 3) / 4;
  void *p = NULL;
  if (clickup)
    return 0;
  if (clickup_failed)
    return -1;
  if (_swix (OS_Module, _INR (0, 0) | _IN (3) | _OUT (2), 6, (int) ((words + 6) * 4), &p)
      || !p)
    {
      clickup_failed = 1;
      return -1;
    }
  clickup = p;
  memcpy (clickup, rdps_clickup_ticker, CODE_BYTES);
  memset (clickup + words, 0, 6 * 4);
  _swix (OS_SynchroniseCodeAreas, _INR (0, 2), 1, clickup,
         (const char *) clickup + CODE_BYTES - 1);
  return 0;
}

/* Stops a release still waiting for button ll and does it now. */
static void
click_cancel (int ll)
{
  volatile int *slot = click_slot (ll);
  if (!slot[1])
    return;
  _swix (OS_RemoveTickerEvent, _INR (0, 1), clickup, slot);
  if (slot[1])
    {
      slot[1] = 0;
      plat_key (ll, 0);
    }
}

int
plat_click (int ll, unsigned up_after_cs)
{
  volatile int *slot;
  if (ll < 0x70 || ll > 0x72 || clickup_init () != 0)
    return -1;
  slot = click_slot (ll);
  click_cancel (ll);
  plat_key (ll, 1);
  slot[0] = ll;
  slot[1] = 1;
  if (_swix (OS_CallAfter, _INR (0, 2), (int) (up_after_cs ? up_after_cs : 1),
             clickup, slot))
    {
      /* No ticker: let go now (a click, though a double-click may then
         reach the Filer as a single one). */
      slot[1] = 0;
      plat_key (ll, 0);
    }
  return 0;
}

void
plat_click_finish (void)
{
  int ll;
  if (!clickup)
    return;
  for (ll = 0x70; ll <= 0x72; ll++)
    click_cancel (ll);
  _swix (OS_Module, _INR (0, 0) | _IN (2), 7, clickup);
  clickup = NULL;
}

/* OS_Byte 121 (PRM p239) with internal key number EOR &80 checks one key:
   R1 = &FF if it's down. Internal keys 0, 1, 2 are Shift, Ctrl and Alt
   (either side; IntKey's table). OS_Byte 202 (p268) with R1 0, R2 &FF
   reads the keyboard status byte without changing it. */
static int
key_down (int internal)
{
  int r1 = 0;
  if (_swix (OS_Byte, _INR (0, 1) | _OUT (1), 121, internal ^ 0x80, &r1))
    return 0;
  return (r1 & 0xff) == 0xff;
}

int
plat_modifiers (void)
{
  int m = 0, status = 0;
  if (key_down (0)) m |= PLAT_SHIFT;
  if (key_down (1)) m |= PLAT_CTRL;
  if (key_down (2)) m |= PLAT_ALT;
  if (_swix (OS_Byte, _INR (0, 2) | _OUT (1), 202, 0, 0xff, &status) == NULL)
    m |= (status & 0xff) << 8;
  return m;
}

/* PointerV 9 (PRM p1313), as RISC OS 5's USB mouse driver reports its
   wheels: R1 vertical (negative = up/away), R2 extra buttons, R3
   horizontal (positive = right). */
void
plat_wheel (int dz, int dw)
{
  _swix (OS_CallAVector, _INR (0, 3) | _IN (9), 9, dz, 0, dw, PointerV);
}

void
plat_set_clients (int n)
{
  char v[12];
  int len = sprintf (v, "%d", n);
  _swix (OS_SetVarVal, _INR (0, 4), "RDPServer$Clients", v, len, 0, 0);
}

unsigned
plat_time_cs (void)
{
  int t = 0;
  _swix (OS_ReadMonotonicTime, _OUT (0), &t);
  return (unsigned) t;
}

/* UnixLib turns "<RDPServer$Dir>/xrdp.ini" into <RDPServer$Dir>.xrdp/ini;
   !Run sets both variables. */
const char *
plat_resources_dir (void)
{
  return "<RDPServer$Dir>";
}

const char *
plat_choices_dir (void)
{
  return "<RDPServer$Choices>";
}
