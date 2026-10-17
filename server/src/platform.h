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

/* The current mode's palette (2 to 256 colour modes): the entries as
   0x00RRGGBB in rgb. Returns how many (0 if it can't be read). Only
   meaningful while output isn't redirected (a sprite's palette then). */
int plat_palette (uint32_t rgb[256]);

/* A system variable's value as a number (def if it isn't set). */
int plat_var_int (const char *name, int def);

/* The RISC OS keyboard layout number (OS_Byte 71: 1 UK, 48 USA...), or
   0 if it can't be read. */
int plat_keyboard (void);

/* Moves the mouse to pixel (x, y), top-left origin, on screen s. */
void plat_mouse_to (const rdps_screen *s, int x, int y);

/* Presses (down = 1) or releases a key or mouse button, by RISC OS
   low-level internal key number (PRM p1247; mouse buttons &70-&72). */
void plat_key (int low_level_key, int down);
/* Puts a byte (in RISC OS's alphabet) into the keyboard buffer, as if
   typed: 0, or -1 if there isn't room. */
int plat_char (int c);
/* RISC OS's alphabet number (OS_Byte 71, 127: 101 Latin1, 111 UTF8). */
int plat_alphabet (void);

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

/* Sound: what the computer plays (all of it, mixed), 16-bit stereo
   little-endian, left first. Start gives the rate (Hz); 0, or -1 if
   there's no sound system to listen to. Read copies exactly bytes (a
   multiple of 4) of what was played since, oldest first, or returns 0 if
   fewer have been played yet; if the reader fell behind, the oldest are
   lost. Mute silences this computer's own output (the capture goes on). */
int plat_sound_rate (unsigned *rate_hz);     /* what the computer plays at */
int plat_sound_start (unsigned *rate_hz);
int plat_sound_read (void *buf, int bytes, int max_behind);
void plat_sound_mute (int on);
/* For the log: what the sound system has done since plat_sound_start. */
typedef struct
{
  unsigned calls;       /* buffers it gave us */
  unsigned with_sound;  /* of those, ones that had sound in them */
  unsigned rate;        /* the frequency it last said (as it gives it) */
  unsigned flags;       /* its last flags */
  unsigned written;     /* bytes put in the ring */
  unsigned before;      /* the output handler before ours (0: none) */
  int before_ss;        /* that was SharedSound's */
  int keep;             /* our SharedSound handler is installed */
} plat_sound_info;
void plat_sound_stats (plat_sound_info *i);
/* 1 while the capture still has the sound (no other program took it). */
int plat_sound_ok (void);
void plat_sound_stop (void);

/* Memory (the module's heap, src/mod/libc.c; 0.2-test31). A session
   keeps a block back (plat_memory_reserve), given up when a claim fails so
   that libxrdp, which doesn't check, still gets its small claims;
   plat_memory_short then says so once (and clears). plat_memory_info: the
   heap's size now and most, and whether RISC OS refused to make it
   bigger. Linux: none of it. */
int plat_memory_reserve (unsigned bytes);
int plat_memory_short (void);
void plat_memory_info (unsigned *used_kb, unsigned *max_kb, int *refused);

/* NEON (the module on RISC OS 5, src/mod/vfpctx.c; 0.2-test28: only
   measured, at start-up). The NEON registers belong to whichever program
   has a VFP context active, so the module makes its own (VFPSupport) and
   switches to it around any NEON work, lazily: plat_neon_enter, then
   plat_neon_leave. plat_neon_start makes it, once: 1 if NEON can be used.
   plat_neon_reset after the server thread was thrown away (switch back
   if it was left entered); plat_neon_stop at the end. Linux: none. */
int plat_neon_start (void);
int plat_neon_enter (void);            /* 0, or -1: no NEON */
void plat_neon_leave (void);
void plat_neon_reset (void);
void plat_neon_stop (void);
/* 32 bpp RISC OS pixels (rgb 0: TBGR, 1: TRGB) to the client's 32 or 16
   bpp, as pixels.c does; pixels a multiple of 8. 0, or -1 if not done. */
int plat_neon_convert (int rgb, int client_bpp, void *dst, const void *src,
                       unsigned pixels);
/* bytes: a multiple of 64 */
void plat_neon_copy (void *dst, const void *src, unsigned bytes);

/* Path of the folder for rsakeys.ini, xrdp.ini and so on, and of the
   folder for things the server writes. */
const char *plat_resources_dir (void);
const char *plat_choices_dir (void);

#endif
