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
  size_t total;                /* TotalScreenSize: all the screen's banks */
  int redirected;              /* 1: output was going to a sprite as this was
                                  read, so only base (DisplayStart) is the
                                  screen's: the rest is the sprite's */
} rdps_screen;

/* Reads the current mode. Returns NULL or the error's text. The module
   can run in the middle of another program's turn, which may have
   switched output to a sprite (OS_SpriteOp 60): the VDU and mode
   variables are then the sprite's (PRM: "The VDU Variables and Mode
   Variables will also reflect the change"), and redirected is set. */
const char *plat_screen (rdps_screen *s);

/* Moves the mouse to pixel (x, y), top-left origin, on screen s. */
void plat_mouse_to (const rdps_screen *s, int x, int y);

/* Presses (down = 1) or releases a key or mouse button, by RISC OS
   low-level internal key number (PRM p1247; mouse buttons &70-&72). */
void plat_key (int low_level_key, int down);

/* A click: presses mouse button ll now, and lets go of it after
   up_after_cs from a ticker event, so the release happens even if another
   program has the machine by then (see clickup.s). Returns 0, or -1 if
   nothing was done (no memory for the routines: the caller presses and
   releases it itself). */
int plat_click (int ll, unsigned up_after_cs);

/* Cancels releases still waiting and does them now (when stopping). */
void plat_click_finish (void);

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
