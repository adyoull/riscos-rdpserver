/* RDPServer: RISC OS screen pixels to the colour depth the client asked
   for. Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_PIXELS_H
#define RDPS_PIXELS_H

#include <stddef.h>
#include <stdint.h>

/* The screen's pixel format, from Log2BPP, NColour and ModeFlags (ROOL
   wiki "Mode Flags": bits 12-15 = 0 is TBGR, red in the low bits; bit 14
   set is TRGB; in 16 bpp, bit 7 with NColour 65535 is 5:6:5). */
typedef enum
{
  PIX_UNSUPPORTED = 0,
  PIX_32_TBGR, PIX_32_TRGB,
  PIX_16_555_TBGR, PIX_16_555_TRGB,
  PIX_16_565_TBGR, PIX_16_565_TRGB
} pix_format;

pix_format pix_format_of (int log2bpp, int ncolour, unsigned modeflags);
int pix_bytes (pix_format f);   /* bytes per screen pixel */

/* Converts w x h pixels (src rows src_stride bytes apart) to the client's
   depth: 8 (the 3:3:2 palette below), 15, 16 (little-endian 16-bit), 24
   or 32 (32-bit 0x00RRGGBB, as libxrdp takes them). dst rows are packed:
   w * 1, 2 or 4 bytes. */
void pix_convert (pix_format f, const unsigned char *src, size_t src_stride,
                  int w, int h, int client_bpp, void *dst);

/* Bytes per pixel libxrdp wants for a client depth. */
int pix_client_bytes (int client_bpp);

/* The 256-entry 3:3:2 palette for 8 bpp clients, 0x00RRGGBB. */
void pix_palette_332 (int palette[256]);

#endif
