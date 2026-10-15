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
  static const int in[] = { 0, 3, 4, 5, 6, 9, 11, 12, 149, 148, 150, -1 };
  int out[11];
  uintptr_t display, start, total;
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
  /* ScreenStart (148) is where the VDU drivers write: a bank of screen
     memory (DisplayStart's or another, double-buffering), or a sprite
     while output is redirected. DisplayStart (149) is what the display
     hardware shows, which redirection doesn't change; TotalScreenSize
     (150) is the screen memory's size (or the sprite's, while
     redirected: either way it's checked against DisplayStart). Output
     goes to the screen only if ScreenStart is within that of
     DisplayStart (written so it can't wrap at 4 GB). */
  display = (uintptr_t) (unsigned) out[8];
  start = (uintptr_t) (unsigned) out[9];
  total = (uintptr_t) (unsigned) out[10];
  s->total = total;
  s->redirected = total != 0 && !((uintptr_t) (start - display + total) < 2 * total);
  return NULL;
}

/* ColourTrans_ReadPalette (PRM: R0 = R1 = -1, the current mode and
   palette; entries &BBGGRR00, all 256 in 256 colour modes). How many
   came from R3, the space left in the buffer, which can only be read one
   way (R2 is "incremented by words written": bytes or words?). */
int
plat_palette (uint32_t rgb[256])
{
  static uint32_t buf[256];
  int left = 0, n, i;
  if (_swix (ColourTrans_ReadPalette, _INR (0, 4) | _OUT (3), -1, -1, (int) (uintptr_t) buf,
             (int) sizeof buf, 0, &left) != NULL)
    return 0;
  if (left < 0 || left > (int) sizeof buf)
    return 0;
  n = ((int) sizeof buf - left) / 4;
  for (i = 0; i < n; i++)
    {
      uint32_t v = buf[i];
      rgb[i] = (((v >> 8) & 255) << 16) | (((v >> 16) & 255) << 8) | ((v >> 24) & 255);
    }
  return n;
}

int
plat_var_int (const char *name, int def)
{
  char v[16];
  int len = -1;
  if (_swix (OS_ReadVarVal, _INR (0, 4) | _OUT (2), name, v, (int) sizeof v - 1, 0, 3, &len)
      != NULL || len <= 0 || len >= (int) sizeof v)
    return def;
  v[len] = 0;
  return atoi (v);
}

/* OS_Byte 71, R1 = 255: read the keyboard layout number (PRM: "returned
   as the actual value", 1:1 with territory numbers). */
int
plat_keyboard (void)
{
  int r1 = 0;
  if (_swix (OS_Byte, _INR (0, 1) | _OUT (1), 71, 255, &r1) != NULL)
    return 0;
  return r1 > 0 && r1 < 128 ? r1 : 0;
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

/* OS_Byte 138 (insert a byte into a buffer), buffer 0: the keyboard.
   Bytes 0 and &80-&FF go in as 0 then the byte, or they'd be taken for
   function keys (the PRM: "Ì" would be F12, "ü" Ctrl-Shift-F12, which
   shuts the desktop down). Only when there's room for both (OS_Byte 128,
   255: bytes in the keyboard buffer; it holds at least 31), with
   interrupts off between, so a key pressed here can't come between
   them. */
int
plat_char (int c)
{
  int used_lo = 0, used_hi = 0, r = -1;
  c &= 0xFF;
  _swix (OS_IntOff, 0);
  if (_swix (OS_Byte, _INR (0, 1) | _OUTR (1, 2), 128, 255, &used_lo, &used_hi) == NULL
      && ((used_lo & 0xFF) | (used_hi << 8)) <= 28)
    {
      if (c == 0 || c >= 0x80)
        _swix (OS_Byte, _INR (0, 2), 138, 0, 0);
      _swix (OS_Byte, _INR (0, 2), 138, 0, c);
      r = 0;
    }
  _swix (OS_IntOn, 0);
  return r;
}

int
plat_alphabet (void)
{
  int a = 0;
  if (_swix (OS_Byte, _INR (0, 1) | _OUT (1), 71, 127, &a) != NULL)
    return 0;
  return a;
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

/* Hardware overlays (VideoOverlay: Reel's video, EGL programs' windows)
   never reach screen memory, so a client sees a black hole where they
   are. Reel and riscos-mesa's EGL read EGL$Overlay at every frame and
   plot ordinarily while it's "off", so it's set to "off" while a client
   is connected, and what was there before (if anything) is put back
   when the client goes, unless something else changed it meanwhile.
   OS_ReadVarVal without R4 = 3 gives the value unexpanded with its
   type (PRM p465). A string is put back as type 4 (literal): type 0
   would GSTrans it a second time (OS_SetVarVal, p489). A number is 4
   bytes, so the buffer is word aligned. A value that can't be read (too
   long, say) is left alone. */
static int ovl_saved;           /* 1 while EGL$Overlay is ours */
static int ovl_had;             /* it existed before */
static int ovl_type;
static int ovl_len;
static int ovl_val[64];         /* 256 bytes, word aligned */

/* Does EGL$Overlay exist? OS_ReadVarVal with R2 < 0 gives R2 = 0 if
   not, else NOT its length, with an error either way (PRM p465), so R2
   is read whatever the error, as UnixLib's __getenv_from_os does: no
   error number is relied on. 0 unset, else the length. */
static int
ovl_length (void)
{
#if defined(__arm__)
  register int r0 __asm__ ("r0") = (int) "EGL$Overlay";
  register int r1 __asm__ ("r1") = 0;
  register int r2 __asm__ ("r2") = -1;
  register int r3 __asm__ ("r3") = 0;
  register int r4 __asm__ ("r4") = 0;
  __asm__ volatile ("swi %[n]"
                    : "+r" (r0), "+r" (r1), "+r" (r2), "+r" (r3), "+r" (r4)
                    : [n] "i" (0x20000 | OS_ReadVarVal)
                    : "lr", "cc", "memory");
  return r2 == 0 ? 0 : ~r2;
#else
  /* the host's fake RISC OS: its OS_ReadVarVal gives R2 without an error */
  _kernel_swi_regs r;
  r.r[0] = (int) "EGL$Overlay";
  r.r[1] = 0;
  r.r[2] = -1;
  r.r[3] = 0;
  r.r[4] = 0;
  if (_kernel_swi (OS_ReadVarVal, &r, &r) != NULL)
    return -1;
  return r.r[2] == 0 ? 0 : ~r.r[2];
#endif
}

/* 1 read, 0 unset, -1 there but unreadable here (too long) */
static int
ovl_read (void *buf, int size, int *len, int *type)
{
  int l = -1, t = 0, n = ovl_length ();
  if (n == 0)
    return 0;
  if (n < 0 || n > size
      || _swix (OS_ReadVarVal, _INR (0, 4) | _OUT (2) | _OUT (4),
                "EGL$Overlay", buf, size, 0, 0, &l, &t) != NULL
      || l < 0 || l > size)
    return -1;
  *len = l;
  *type = t;
  return 1;
}

static void
overlays_off (int off)
{
  if (off && !ovl_saved)
    {
      ovl_had = ovl_read (ovl_val, sizeof ovl_val, &ovl_len, &ovl_type);
      if (ovl_had < 0 || (ovl_had && ovl_type == 16))
        return;                 /* unreadable, or a code variable: not ours to touch */
      _swix (OS_SetVarVal, _INR (0, 4), "EGL$Overlay", "off", 3, 0, 4);
      ovl_saved = 1;
    }
  else if (!off && ovl_saved)
    {
      int cur[2], len, type;
      ovl_saved = 0;
      if (ovl_read (cur, sizeof cur, &len, &type) != 1 || type != 0 || len != 3
          || memcmp (cur, "off", 3) != 0)
        return;                 /* changed by someone else meanwhile */
      if (ovl_had)
        _swix (OS_SetVarVal, _INR (0, 4), "EGL$Overlay", ovl_val, ovl_len, 0,
               ovl_type == 0 ? 4 : ovl_type);
      else
        _swix (OS_SetVarVal, _INR (0, 4), "EGL$Overlay", "", -1, 0, 0);
    }
}

void
plat_set_clients (int n)
{
  char v[12];
  int len = sprintf (v, "%d", n);
  _swix (OS_SetVarVal, _INR (0, 4), "RDPServer$Clients", v, len, 0, 0);
  overlays_off (n > 0);
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
