/* RDPServer: characters from key presses for the login screen, by the
   RISC OS keyboard layout in use (OS_Byte 71), as the rest of RDPServer
   types through RISC OS's own layout. PC scan codes (set 1, the physical
   key), Latin-1 characters.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_KBDLAYOUT_H
#define RDPS_KBDLAYOUT_H

/* RISC OS keyboard (= territory) numbers with a table here. */
#define KBD_UK 1
#define KBD_FRANCE 6
#define KBD_GERMANY 7
#define KBD_NETHERLANDS 34
#define KBD_USA 48

/* Whether there's a table for this keyboard number. */
int kbd_known (int keyboard);
/* A name for the Log ("UK", ...). */
const char *kbd_name (int keyboard);
/* The RISC OS keyboard number for a client's layout id (0x809 UK...),
   or 0 if none of ours. */
int kbd_from_client (int keylayout);

/* The Latin-1 character for scan code code (not an E0 key) with Shift
   and AltGr as given and Caps Lock on or off, or 0 (none, a dead key, or
   a key that doesn't type). An unknown keyboard is taken as USA. */
unsigned kbd_char (int keyboard, int code, int shift, int altgr, int caps);

/* A character a client sent as itself (a Unicode keyboard event) as
   RISC OS's Latin-1 alphabet has it: Latin-1 itself, the extra
   characters RISC OS puts at &80-&9F (the euro, typographic quotes and
   dashes, Œ œ and so on), and controls (Return, Backspace, Tab, Escape,
   Delete). -1 if it hasn't got it. */
int kbd_unicode_char (unsigned u);

/* The bytes RISC OS's alphabet (OS_Byte 71, 127: 101 Latin1, 111 UTF8,
   112 Latin9, ...) has for a character a client sent as itself: up to 4
   in out, their number returned; 0 if it hasn't got it. Latin1 (and an
   unknown alphabet) as kbd_unicode_char; UTF8 its UTF-8; Latin9 ISO
   8859-15's differences from Latin-1; any other, ASCII and controls. */
#define KBD_ALPHABET_LATIN1 101
#define KBD_ALPHABET_UTF8 111
#define KBD_ALPHABET_LATIN9 112
int kbd_unicode_bytes (unsigned u, int alphabet, unsigned char out[4]);

#endif
