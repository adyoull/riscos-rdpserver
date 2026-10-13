/* RDPServer: plat_riscos.c on Linux against the fake RISC OS.
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "fake.h"
#include "swis.h"
#include "../src/platform.h"

static unsigned char screen[16];
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

int main (void)
{
  rdps_screen s;

  fake_reset ();
  fake_swi (OS_ReadVduVariables, "OS_ReadVduVariables", vdu);
  fake_swi (OS_Word, "OS_Word", word);
  fake_swi (OS_CallAVector, "OS_CallAVector", callavector);
  fake_swi (OS_SetVarVal, "OS_SetVarVal", setvarval);
  fake_swi (OS_Byte, "OS_Byte", byte);

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
  return fake_done ("RDPServer plat_riscos (fake RISC OS)");
}
