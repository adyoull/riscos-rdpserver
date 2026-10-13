/* RDPServer: RDP keyboard scancodes (PC set 1, with the extended flag) to
   RISC OS low-level internal key numbers (PRM p1247-1249).
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_KEYMAP_H
#define RDPS_KEYMAP_H

/* Returns the low-level key number, or -1 for a key RISC OS hasn't got. */
int keymap_lookup (int scancode, int extended);

#endif
