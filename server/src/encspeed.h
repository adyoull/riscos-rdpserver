/* RDPServer: how fast this computer encodes the screen (encspeed.c).
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_ENCSPEED_H
#define RDPS_ENCSPEED_H

#include "platform.h"
#include "pixels.h"

typedef struct
{
  unsigned convert_tps, compress_tps, mppc_tps;   /* 64x64 tiles a second */
  unsigned rle_kb, mppc_kb;     /* a whole screen of tiles like these */
  unsigned screen_tiles, tiles; /* in a whole screen; measured */
} rdps_enc_speed;

/* Times converting the screen's middle 64 rows to cbpp, compressing them
   and MPPC, cs centiseconds each. 0, or -1 (a palette mode, a screen
   under 64 pixels, out of memory). */
int rdps_encode_speed (const rdps_screen *scr, pix_format fmt, int cbpp, unsigned cs,
                       rdps_enc_speed *r);

/* milliseconds for a whole screen at tps tiles a second */
unsigned rdps_enc_ms (unsigned screen_tiles, unsigned tps);

#endif
