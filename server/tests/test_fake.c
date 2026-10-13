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
static char var_name[64], var_val[16];

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

static const _kernel_oserror *setvarval (int swi, int r[10])
{
  (void) swi;
  snprintf (var_name, sizeof var_name, "%s", (const char *) (intptr_t) r[0]);
  snprintf (var_val, sizeof var_val, "%.*s", r[2], (const char *) (intptr_t) r[1]);
  return NULL;
}

/* OS_Byte 121 (one key, X = internal key EOR &80) and 202. */
static int held[3], kbstatus = 0x30, byte202_r2 = -1;
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

  /* Held buttons (drags): down, a watchdog, pushed back while we run. */
  slot = (int *) (rma_block + 36);    /* a new block since finish */
  calls = keyv_calls;
  callafter_calls = 0;
  plat_button_hold (0x70, 50);
  CHECK (keyv_calls == calls + 1 && keyv_r0 == 2 && keyv_r1 == 0x70, "hold: Select down");
  CHECK (callafter_calls == 1 && callafter_r[0] == 50 && callafter_r[2] == (int) (intptr_t) slot,
         "with a 50 cs watchdog");
  remove_calls = 0;
  CHECK (plat_button_keep (0x70, 50) == 0 && remove_calls == 1 && callafter_calls == 2,
         "keep: the watchdog is moved back (removed, set again)");
  slot[1] = 0;                        /* it went off: a program held the desktop */
  CHECK (plat_button_keep (0x70, 50) == -1, "keep reports that the watchdog let go");
  calls = keyv_calls;
  plat_button_release (0x70);
  CHECK (keyv_calls == calls + 1 && keyv_r0 == 1 && keyv_r1 == 0x70, "release: KeyV up (harmless if already up)");
  plat_button_hold (0x70, 50);
  remove_calls = 0;
  plat_button_release (0x70);
  CHECK (remove_calls == 1 && slot[1] == 0, "release cancels a watchdog still waiting");
  callafter_fail = 1;
  plat_button_hold (0x71, 50);
  CHECK (plat_button_keep (0x71, 50) == 0, "no ticker for the watchdog: held, not reported as let go");
  callafter_fail = 0;
  plat_button_release (0x71);
  plat_click_finish ();

  rma_fail = 1;
  rma_freed = 0;
  calls = keyv_calls;
  CHECK (plat_click (0x70, 2) == -1 && keyv_calls == calls, "no RMA: -1, nothing pressed");
  rma_fail = 0;
  CHECK (plat_click (0x70, 2) == -1, "and it doesn't keep trying");
}

int main (void)
{
  rdps_screen s;

  fake_reset ();
  fake_swi (OS_ReadVduVariables, "OS_ReadVduVariables", vdu);
  fake_swi (OS_Word, "OS_Word", word);
  fake_swi (OS_CallAVector, "OS_CallAVector", callavector);
  fake_swi (OS_SetVarVal, "OS_SetVarVal", setvarval);
  fake_swi (OS_Byte, "OS_Byte", byte);
  fake_swi (OS_Module, "OS_Module", module);
  fake_swi (OS_SynchroniseCodeAreas, "OS_SynchroniseCodeAreas", sync_code);
  fake_swi (OS_CallAfter, "OS_CallAfter", callafter);
  fake_swi (OS_RemoveTickerEvent, "OS_RemoveTickerEvent", remove_ev);
  fake_swi (OS_RemoveCallBack, "OS_RemoveCallBack", remove_ev);

  CHECK (plat_screen (&s) == NULL, "the screen is read");
  CHECK (s.width == 1920 && s.height == 1080, "1920 x 1080 from the window limits");
  CHECK (s.log2bpp == 5 && s.ncolour == -1 && s.modeflags == 0x40, "the format");
  CHECK (s.stride == 7680 && s.base == screen, "LineLength and DisplayStart");
  CHECK (s.xeig == 1 && s.yeig == 2, "eig factors");

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
  CHECK (strcmp (var_name, "RDPServer$Clients") == 0 && strcmp (var_val, "1") == 0,
         "RDPServer$Clients set to 1");
  plat_set_clients (0);
  CHECK (strcmp (var_val, "0") == 0, "and back to 0");

  CHECK (strcmp (plat_resources_dir (), "<RDPServer$Dir>") == 0, "resources in the app");
  CHECK (strcmp (plat_choices_dir (), "<RDPServer$Choices>") == 0, "keys in Choices");
  test_clicks ();
  return fake_done ("RDPServer plat_riscos (fake RISC OS)");
}
