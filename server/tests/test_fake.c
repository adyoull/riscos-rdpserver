/* RDPServer: plat_riscos.c on Linux against the fake RISC OS.
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "fake.h"
#include "swis.h"
#include "../src/platform.h"

/* Stand-ins for clickup.s's routines (ARM code; tests/test_clickup.py
   runs the real ones): 36 bytes of ticker routine. */
__asm__ (".data\n.globl rdps_clickup_ticker\n"
         ".globl rdps_clickup_end\nrdps_clickup_ticker: .fill 36,1,0xA1\n"
         "rdps_clickup_end:\n.text");
extern const unsigned char rdps_clickup_ticker[];

static unsigned char screen[16];
static int redirect;              /* output switched to a sprite */
static unsigned char *rma_block;
static int rma_size, rma_freed, rma_fail, sync_r[3], callafter_r[3], callafter_calls,
  callafter_fail, removed_ticker[2], removed_callback[2], remove_calls;

static const _kernel_oserror no_rma = { 0x101, "No room in RMA" };
static const _kernel_oserror no_ticker = { 0x1E6, "No room" };

static const _kernel_oserror *module (int swi, int r[10])
{
  (void) swi;
  if (r[0] == 6)
    {
      if (rma_fail)
        return &no_rma;
      rma_size = r[3];
      rma_block = calloc (1, (size_t) r[3]);
      r[2] = (int) (intptr_t) rma_block;
      return NULL;
    }
  if (r[0] == 7 && (unsigned char *) (intptr_t) r[2] == rma_block)
    {
      rma_freed = 1;
      return NULL;
    }
  abort ();
}
static const _kernel_oserror *sync_code (int swi, int r[10]) { (void) swi; memcpy (sync_r, r, sizeof sync_r); return NULL; }
static const _kernel_oserror *callafter (int swi, int r[10])
{
  (void) swi;
  memcpy (callafter_r, r, sizeof callafter_r);
  callafter_calls++;
  return callafter_fail ? &no_ticker : NULL;
}
static const _kernel_oserror *remove_ev (int swi, int r[10])
{
  remove_calls++;
  if (swi == OS_RemoveTickerEvent)
    memcpy (removed_ticker, r, sizeof removed_ticker);
  else
    memcpy (removed_callback, r, sizeof removed_callback);
  return NULL;
}
static unsigned char last21[5];
static int keyv_r0, keyv_r1, keyv_calls;
static int ptrv_r[4], ptrv_calls;

static const _kernel_oserror *vdu (int swi, int r[10])
{
  const int *in = (const int *) (intptr_t) r[0];
  int *out = (int *) (intptr_t) r[1];
  (void) swi;
  for (; *in != -1; in++, out++)
    switch (*in)
      {
      case 0: *out = 0x40; break;        /* hardware scroll off */
      case 3: *out = -1; break;
      case 4: *out = 1; break;
      case 5: *out = 2; break;
      case 6: *out = 7680; break;
      case 9: *out = 5; break;
      case 11: *out = 1919; break;
      case 12: *out = 1079; break;
      case 149: *out = (int) (intptr_t) screen; break;
      /* a sprite is far from screen memory (RMA, a dynamic area) */
      case 148: *out = (int) ((intptr_t) screen ^ (redirect ? 0x40000000 : 0)); break;
      case 150: *out = 8 << 20; break;
      default:
        fprintf (stderr, "unexpected VDU variable %d\n", *in);
        abort ();
      }
  return NULL;
}

static const _kernel_oserror *word (int swi, int r[10])
{
  (void) swi;
  if (r[0] != 21)
    abort ();
  memcpy (last21, (const void *) (intptr_t) r[1], 5);
  return NULL;
}

/* System variables (PRM p465, p489): types 0 (GSTrans isn't done here,
   but the type used is recorded), 1, 2 and 4 (stored as a string, 0). */
#define NVARS 4
static struct { char name[40], val[400]; int len, type, set; } vars[NVARS];
static int last_set_type = -1;
static int var_find (const char *name, int make)
{
  int i, free_i = -1;
  for (i = 0; i < NVARS; i++)
    if (vars[i].set && strcmp (vars[i].name, name) == 0)
      return i;
    else if (!vars[i].set && free_i < 0)
      free_i = i;
  if (!make || free_i < 0)
    {
      if (make)
        abort ();
      return -1;
    }
  snprintf (vars[free_i].name, sizeof vars[free_i].name, "%s", name);
  vars[free_i].set = 1;
  return free_i;
}
static const char *var_get (const char *name)
{
  int i = var_find (name, 0);
  return i < 0 ? NULL : vars[i].val;
}
static const _kernel_oserror *setvarval (int swi, int r[10]);
static void var_put (const char *name, const char *v, int type)
{
  int r[10] = { (intptr_t) name, (intptr_t) v, (int) strlen (v), 0, type };
  setvarval (0, r);
}
static const _kernel_oserror var_missing = { 0x1E7, "System variable not found (number made up)" };
static const _kernel_oserror var_overflow = { 0x1E4, "Buffer overflow" };
static const _kernel_oserror *setvarval (int swi, int r[10])
{
  const char *name = (const char *) (intptr_t) r[0];
  int i;
  (void) swi;
  if (r[2] < 0)
    {
      if ((i = var_find (name, 0)) < 0)
        return &var_missing;
      vars[i].set = 0;
      return NULL;
    }
  if (r[4] != 0 && r[4] != 1 && r[4] != 2 && r[4] != 4)
    abort ();
  if (r[2] >= (int) sizeof vars[0].val)
    abort ();
  last_set_type = r[4];
  i = var_find (name, 1);
  memcpy (vars[i].val, (const char *) (intptr_t) r[1], (size_t) r[2]);
  vars[i].val[r[2]] = 0;
  vars[i].len = r[2];
  vars[i].type = r[4] == 4 ? 0 : r[4];
  return NULL;
}
static const _kernel_oserror *readvarval (int swi, int r[10])
{
  int i = var_find ((const char *) (intptr_t) r[0], 0);
  (void) swi;
  if (r[2] < 0)
    {
      /* RISC OS also gives an error here, which plat_riscos.c ignores
         (it reads R2 whatever happens): the fake's _kernel_swi wouldn't
         pass R2 back with one. */
      r[2] = i < 0 ? 0 : ~vars[i].len;
      return NULL;
    }
  if (r[3] != 0 || r[4] == 3 || i < 0)
    {
      r[2] = 0;
      return &var_missing;
    }
  if (vars[i].len > r[2])
    {
      r[2] = ~vars[i].len;
      return &var_overflow;
    }
  memcpy ((char *) (intptr_t) r[1], vars[i].val, (size_t) vars[i].len);
  r[2] = vars[i].len;
  r[4] = vars[i].type;
  return NULL;
}

/* OS_Byte 121 (one key, X = internal key EOR &80) and 202. */
static int held[3], kbstatus = 0x30, byte202_r2 = -1, byte138_r1 = -1, byte138_r2 = -1;
static char inserted[64];               /* the bytes OS_Byte 138 put in, "I" for
                                           interrupts off, "i" on again */
static int kb_used, ints_off;
static const _kernel_oserror *intoff (int swi, int r[10]) { (void) swi; (void) r; ints_off = 1; strcat (inserted, "I "); return NULL; }
static const _kernel_oserror *inton (int swi, int r[10]) { (void) swi; (void) r; ints_off = 0; strcat (inserted, "i "); return NULL; }
static const _kernel_oserror *byte (int swi, int r[10])
{
  (void) swi;
  if (r[0] == 121)
    {
      int k = r[1] ^ 0x80;
      if (k < 0 || k > 2)
        abort ();
      r[1] = held[k] ? 0xff : 0;
      return NULL;
    }
  if (r[0] == 202)
    {
      byte202_r2 = r[2];
      if (r[1] != 0 || r[2] != 0xff)
        abort ();          /* must only read, never change it */
      r[1] = kbstatus;
      return NULL;
    }
  if (r[0] == 138)
    {
      byte138_r1 = r[1];
      byte138_r2 = r[2];
      snprintf (inserted + strlen (inserted), sizeof inserted - strlen (inserted), "%s%02X ",
                ints_off ? "" : "!", r[2] & 0xFF);
      kb_used++;
      return NULL;
    }
  if (r[0] == 128)
    {
      if (r[1] != 255)
        abort ();          /* only the keyboard buffer's count */
      r[1] = kb_used & 0xFF;
      r[2] = kb_used >> 8;
      return NULL;
    }
  if (r[0] == 71 && r[1] == 127)
    {
      r[1] = 111;          /* the alphabet: UTF8 */
      return NULL;
    }
  if (r[0] == 71)
    {
      if (r[1] != 255)
        abort ();          /* must only read the keyboard number */
      r[1] = 34;           /* the Netherlands */
      return NULL;
    }
  abort ();
}

static const _kernel_oserror *callavector (int swi, int r[10])
{
  (void) swi;
  if (r[9] == 38)
    {
      memcpy (ptrv_r, r, sizeof ptrv_r);
      ptrv_calls++;
      return NULL;
    }
  if (r[9] != 19)
    abort ();
  keyv_r0 = r[0];
  keyv_r1 = r[1];
  keyv_calls++;
  return NULL;
}

/* Clicks: the button goes down now; a ticker event lets go of it. */
static void test_clicks (void)
{
  int calls, *slot;
  calls = keyv_calls;
  CHECK (plat_click (0x70, 2) == 0, "a click");
  CHECK (rma_block && rma_size >= 36 + 24, "an RMA block for the routines and 3 pairs");
  CHECK (memcmp (rma_block, rdps_clickup_ticker, 36) == 0, "the routines copied into it");
  CHECK (sync_r[0] == 1 && sync_r[1] == (int) (intptr_t) rma_block
         && sync_r[2] == (int) (intptr_t) rma_block + 35, "OS_SynchroniseCodeAreas over the new code");
  CHECK (keyv_calls == calls + 1 && keyv_r0 == 2 && keyv_r1 == 0x70, "KeyV: Select down now");
  slot = (int *) (rma_block + 36);
  CHECK (callafter_r[0] == 2 && callafter_r[1] == (int) (intptr_t) rma_block
         && callafter_r[2] == (int) (intptr_t) slot, "OS_CallAfter 2 cs: the ticker routine, R12 = its pair");
  CHECK (slot[0] == 0x70 && slot[1] == 1, "the pair: key &70, pending");

  slot[1] = 0;                        /* the callback has run */
  remove_calls = 0;
  plat_click (0x70, 2);
  CHECK (remove_calls == 0 && callafter_calls == 2, "done: the next click needs no cancelling");

  plat_click (0x72, 2);               /* Adjust, left pending */
  CHECK (callafter_r[2] == (int) (intptr_t) (slot + 4) && slot[4] == 0x72, "Adjust has its own pair");
  calls = keyv_calls;
  slot[1] = 0;
  remove_calls = 0;
  plat_click_finish ();
  CHECK (removed_ticker[0] == (int) (intptr_t) rma_block && removed_ticker[1] == (int) (intptr_t) (slot + 4),
         "stopping: the waiting ticker event is removed");
  CHECK (remove_calls == 1, "(only the ticker event: there's no callback)");
  CHECK (keyv_calls == calls + 1 && keyv_r0 == 1 && keyv_r1 == 0x72, "and Adjust let go of now");
  CHECK (rma_freed, "then the RMA block is freed");

  callafter_fail = 1;
  calls = keyv_calls;
  CHECK (plat_click (0x71, 2) == 0 && keyv_calls == calls + 2 && keyv_r0 == 1 && keyv_r1 == 0x71,
         "no ticker: down and straight back up");
  callafter_fail = 0;
  plat_click_finish ();

  rma_fail = 1;
  rma_freed = 0;
  calls = keyv_calls;
  CHECK (plat_click (0x70, 2) == -1 && keyv_calls == calls, "no RMA: -1, nothing pressed");
  rma_fail = 0;
  CHECK (plat_click (0x70, 2) == -1, "and it doesn't keep trying");
}

/* ColourTrans_ReadPalette: 4 entries, &BBGGRR00 */
static int ctrp_r[5], ctrp_fail;
static const _kernel_oserror no_ct = { 0x1, "ColourTrans not there" };
static const _kernel_oserror *readpalette (int swi, int r[10])
{
  static const uint32_t pal[4] = { 0x00000000u, 0x0000FF00u, 0x00FF0000u, 0x563412F0u };
  (void) swi;
  memcpy (ctrp_r, r, sizeof ctrp_r);
  if (ctrp_fail)
    return &no_ct;
  memcpy ((void *) (intptr_t) r[2], pal, sizeof pal);
  r[2] += 4;      /* R2 "incremented by words written": not relied on */
  r[3] -= (int) sizeof pal;
  return NULL;
}

int main (void)
{
  rdps_screen s;

  fake_reset ();
  fake_swi (OS_ReadVduVariables, "OS_ReadVduVariables", vdu);
  fake_swi (OS_Word, "OS_Word", word);
  fake_swi (OS_CallAVector, "OS_CallAVector", callavector);
  fake_swi (OS_SetVarVal, "OS_SetVarVal", setvarval);
  fake_swi (OS_ReadVarVal, "OS_ReadVarVal", readvarval);
  fake_swi (OS_Byte, "OS_Byte", byte);
  fake_swi (OS_IntOff, "OS_IntOff", intoff);
  fake_swi (OS_IntOn, "OS_IntOn", inton);
  fake_swi (OS_Module, "OS_Module", module);
  fake_swi (OS_SynchroniseCodeAreas, "OS_SynchroniseCodeAreas", sync_code);
  fake_swi (OS_CallAfter, "OS_CallAfter", callafter);
  fake_swi (OS_RemoveTickerEvent, "OS_RemoveTickerEvent", remove_ev);
  fake_swi (OS_RemoveCallBack, "OS_RemoveCallBack", remove_ev);
  fake_swi (ColourTrans_ReadPalette, "ColourTrans_ReadPalette", readpalette);

  CHECK (plat_screen (&s) == NULL, "the screen is read");
  CHECK (s.width == 1920 && s.height == 1080, "1920 x 1080 from the window limits");
  CHECK (s.log2bpp == 5 && s.ncolour == -1 && s.modeflags == 0x40, "the format");
  CHECK (s.stride == 7680 && s.base == screen, "LineLength and DisplayStart");
  CHECK (s.xeig == 1 && s.yeig == 2, "eig factors");
  CHECK (!s.redirected, "output to the screen: ScreenStart is DisplayStart's bank");
  redirect = 1;
  CHECK (plat_screen (&s) == NULL && s.redirected && s.base == screen,
         "output to a sprite: redirected, and base still DisplayStart");
  redirect = 0;

  {
    uint32_t pal[256];
    memset (pal, 0x77, sizeof pal);
    CHECK (plat_palette (pal) == 4, "the palette: 4 entries read");
    CHECK (ctrp_r[0] == -1 && ctrp_r[1] == -1 && ctrp_r[4] == 0 && ctrp_r[3] == 1024,
           "ColourTrans_ReadPalette: current mode and palette, a 256-word buffer, no flags");
    CHECK (pal[0] == 0 && pal[1] == 0xff0000 && pal[2] == 0x00ff00 && pal[3] == 0x123456,
           "&BBGGRR00 to 0x00RRGGBB");
    CHECK (pal[4] == 0x77777777, "entries past those read untouched");
    ctrp_fail = 1;
    CHECK (plat_palette (pal) == 0, "an error: none");
    ctrp_fail = 0;
  }
  CHECK (plat_keyboard () == 34, "the keyboard layout: OS_Byte 71, 255 (read)");

  /* Top-left pixel (0,0) is the top of the screen: OS y = (1080-1) << 2. */
  plat_mouse_to (&s, 0, 0);
  CHECK (last21[0] == 3, "OS_Word 21,3");
  CHECK (last21[1] == 0 && last21[2] == 0, "x 0");
  CHECK ((last21[3] | (last21[4] << 8)) == 1079 * 4, "y = 4316 OS units");
  plat_mouse_to (&s, 1919, 1079);
  CHECK ((last21[1] | (last21[2] << 8)) == 1919 * 2, "x = 3838 OS units");
  CHECK (last21[3] == 0 && last21[4] == 0, "bottom row: y 0");

  plat_key (0x70, 1);
  CHECK (keyv_calls == 1 && keyv_r0 == 2 && keyv_r1 == 0x70, "Select pressed: KeyV 2, &70");
  plat_key (0x3C, 0);
  CHECK (keyv_r0 == 1 && keyv_r1 == 0x3C, "A released: KeyV 1, &3C");

  inserted[0] = 0;
  CHECK (plat_char ('h') == 0 && byte138_r1 == 0 && strcmp (inserted, "I 68 i ") == 0
         && keyv_calls == 2, "a character: OS_Byte 138 into buffer 0 (the keyboard), not KeyV");
  inserted[0] = 0;
  CHECK (plat_char (0xFC) == 0 && strcmp (inserted, "I 00 FC i ") == 0,
         "a top-bit one goes in as 0 then it (not Ctrl-Shift-F12), interrupts off between");
  inserted[0] = 0;
  CHECK (plat_char (0) == 0 && strcmp (inserted, "I 00 00 i ") == 0, "and so does 0");
  kb_used = 29;
  inserted[0] = 0;
  CHECK (plat_char ('x') == -1 && strcmp (inserted, "I i ") == 0,
         "the keyboard buffer nearly full: nothing put in, -1");
  kb_used = 0;
  CHECK (plat_alphabet () == 111, "the alphabet: OS_Byte 71, 127");

  plat_wheel (-2, 1);
  CHECK (ptrv_calls == 1 && ptrv_r[0] == 9 && ptrv_r[1] == -2 && ptrv_r[2] == 0
         && ptrv_r[3] == 1, "wheel: PointerV 9, dz in R1, buttons 0 in R2, dw in R3");
  CHECK (keyv_calls == 2, "the wheel doesn't touch KeyV");

  CHECK (plat_modifiers () == (0x30 << 8), "nothing held: 0, status byte in bits 8-15");
  CHECK (byte202_r2 == 0xff, "OS_Byte 202 reads (R1 0, R2 &FF)");
  held[0] = 1;
  CHECK (plat_modifiers () == (PLAT_SHIFT | (0x30 << 8)), "Shift: OS_Byte 121, &80");
  held[0] = 0; held[1] = 1;
  CHECK ((plat_modifiers () & 7) == PLAT_CTRL, "Ctrl: &81");
  held[1] = 0; held[2] = 1;
  CHECK ((plat_modifiers () & 7) == PLAT_ALT, "Alt: &82");
  held[2] = 0;

  plat_set_clients (1);
  CHECK (var_get ("RDPServer$Clients") && strcmp (var_get ("RDPServer$Clients"), "1") == 0,
         "RDPServer$Clients set to 1");
  CHECK (var_get ("EGL$Overlay") && strcmp (var_get ("EGL$Overlay"), "off") == 0,
         "EGL$Overlay off while a client is connected (it was unset)");
  plat_set_clients (0);
  CHECK (strcmp (var_get ("RDPServer$Clients"), "0") == 0, "and back to 0");
  CHECK (var_get ("EGL$Overlay") == NULL, "EGL$Overlay unset again");

  var_put ("EGL$Overlay", "on", 0);
  plat_set_clients (1);
  plat_set_clients (1);
  CHECK (strcmp (var_get ("EGL$Overlay"), "off") == 0, "the user's EGL$Overlay on: off with a client");
  plat_set_clients (0);
  CHECK (strcmp (var_get ("EGL$Overlay"), "on") == 0, "and their 'on' put back (a second 1 didn't lose it)");

  plat_set_clients (1);
  var_put ("EGL$Overlay", "yes", 0);
  plat_set_clients (0);
  CHECK (strcmp (var_get ("EGL$Overlay"), "yes") == 0,
         "EGL$Overlay changed while a client was connected: left as it was changed");

  var_put ("EGL$Overlay", "<x|y>", 4);
  plat_set_clients (1);
  plat_set_clients (0);
  CHECK (strcmp (var_get ("EGL$Overlay"), "<x|y>") == 0 && last_set_type == 4,
         "a string is put back literally (type 4, not GSTrans'd again)");

  var_put ("EGL$Overlay", "<Obey$Dir>", 2);
  plat_set_clients (1);
  plat_set_clients (0);
  CHECK (strcmp (var_get ("EGL$Overlay"), "<Obey$Dir>") == 0 && last_set_type == 2,
         "a macro is put back as a macro");

  {
    char big[301];
    memset (big, 'a', 300);
    big[300] = 0;
    var_put ("EGL$Overlay", big, 0);
    plat_set_clients (1);
    CHECK (strcmp (var_get ("EGL$Overlay"), big) == 0,
           "a value too long to keep is left alone (not set to off)");
    plat_set_clients (0);
    CHECK (strcmp (var_get ("EGL$Overlay"), big) == 0, "nor deleted afterwards");
  }

  CHECK (strcmp (plat_resources_dir (), "<RDPServer$Dir>") == 0, "resources in the app");
  CHECK (strcmp (plat_choices_dir (), "<RDPServer$Choices>") == 0, "keys in Choices");
  test_clicks ();
  return fake_done ("RDPServer plat_riscos (fake RISC OS)");
}
