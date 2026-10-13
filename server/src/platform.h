/* RDPServer: what the server needs from the machine it runs on. One
   version talks to RISC OS (plat_riscos.c); the other is a fake screen for
   testing on Linux with real RDP clients (plat_host.c).

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_PLATFORM_H
#define RDPS_PLATFORM_H

#include <stdint.h>
#include <stddef.h>

/* The screen as it is now. */
typedef struct
{
  int width, height;           /* pixels */
  int log2bpp;                 /* 4 = 16 bpp, 5 = 32 bpp */
  int ncolour;                 /* NColour mode variable */
  unsigned modeflags;          /* ModeFlags mode variable */
  int xeig, yeig;              /* OS units per pixel = 1 << eig */
  size_t stride;               /* bytes from one row to the next */
  const unsigned char *base;   /* the top row (the displayed bank) */
} rdps_screen;

/* Reads the current mode. Returns NULL or the error's text. */
const char *plat_screen (rdps_screen *s);

/* Moves the mouse to pixel (x, y), top-left origin, on screen s. */
void plat_mouse_to (const rdps_screen *s, int x, int y);

/* Presses (down = 1) or releases a key or mouse button, by RISC OS
   low-level internal key number (PRM p1247; mouse buttons &70-&72). */
void plat_key (int low_level_key, int down);

/* Which of Shift, Ctrl and Alt RISC OS thinks are held down (by any
   keyboard: ours or one plugged in), as PLAT_SHIFT | PLAT_CTRL | PLAT_ALT,
   with the keyboard status byte (OS_Byte 202) in bits 8-15. */
#define PLAT_SHIFT 1
#define PLAT_CTRL 2
#define PLAT_ALT 4
int plat_modifiers (void);

/* Turns the scroll wheel: dz notches (negative = up/away), dw notches
   sideways (positive = right). */
void plat_wheel (int dz, int dw);

/* Tells other programs how many clients are watching (RDPServer$Clients),
   so programs using hardware video overlays, which a screen copy can't
   see, can draw in the ordinary way instead. */
void plat_set_clients (int n);

/* Centiseconds, monotonic. */
unsigned plat_time_cs (void);

/* Path of the folder for rsakeys.ini, xrdp.ini and so on, and of the
   folder for things the server writes. */
const char *plat_resources_dir (void);
const char *plat_choices_dir (void);

#endif
