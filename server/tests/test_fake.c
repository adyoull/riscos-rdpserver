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

static const _kernel_oserror *callavector (int swi, int r[10])
{
  (void) swi;
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

  CHECK (strcmp (plat_resources_dir (), "<RDPServer$Dir>") == 0, "resources in the app");
  CHECK (strcmp (plat_choices_dir (), "<RDPServer$Choices>") == 0, "keys in Choices");
  return fake_done ("RDPServer plat_riscos (fake RISC OS)");
}
