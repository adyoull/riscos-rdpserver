/* RDPServer: RISC OS screen pixels to the colour depth the client asked
   for. Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_PIXELS_H
#define RDPS_PIXELS_H

#include <stddef.h>
#include <stdint.h>

/* The screen's pixel format, from Log2BPP, NColour and ModeFlags (ROOL
   wiki "Mode Flags": bits 12-13 = 0 is RGB, red in the low bits (TBGR);
   bit 14 set is TRGB; bit 15 is alpha, treated as T; in 16 bpp, NColour
   4095 is 4:4:4, and bit 7 with NColour 65535 is 5:6:5). 1, 2, 4 and 8
   bpp (2 to 256 colours) go through the palette (pix_set_palette); their
   pixels are packed the RISC OS way, the leftmost in the lowest bits. */
typedef enum
{
  PIX_UNSUPPORTED = 0,
  PIX_32_TBGR, PIX_32_TRGB,
  PIX_16_555_TBGR, PIX_16_555_TRGB,
  PIX_16_565_TBGR, PIX_16_565_TRGB,
  PIX_16_444_TBGR, PIX_16_444_TRGB,
  PIX_8_PAL, PIX_4_PAL, PIX_2_PAL, PIX_1_PAL
} pix_format;

pix_format pix_format_of (int log2bpp, int ncolour, unsigned modeflags);
int pix_bits (pix_format f);    /* bits per screen pixel (0: unsupported) */
int pix_bytes (pix_format f);   /* bytes per screen pixel (0: less than one) */
int pix_is_palette (pix_format f);

/* The screen's palette for the palette formats: n entries, 0x00RRGGBB
   (missing ones are black). */
void pix_set_palette (const uint32_t *rgb, int n);

/* Converts w x h pixels (src rows src_stride bytes apart; for formats
   of under 8 bits a pixel, src is the byte holding the first one, which
   must be its lowest bits) to the client's depth: 8 (the 3:3:2 palette below), 15, 16 (little-endian 16-bit), 24
   or 32 (32-bit 0x00RRGGBB, as libxrdp takes them). dst rows are packed:
   w * 1, 2 or 4 bytes. */
void pix_convert (pix_format f, const unsigned char *src, size_t src_stride,
                  int w, int h, int client_bpp, void *dst);

/* Bytes per pixel libxrdp wants for a client depth. */
int pix_client_bytes (int client_bpp);

/* The 256-entry 3:3:2 palette for 8 bpp clients, 0x00RRGGBB. */
void pix_palette_332 (int palette[256]);

/* Whether w x h pixels as pix_convert made them (packed, the client's
   depth cbpp) are all one colour; if so, *color is it as an OpaqueRect
   order takes it: the 8-bit index, the 15/16-bit value, or for 24/32 bpp
   red, green, blue from the lowest byte up (bitmaps hold 0x00RRGGBB). p
   must be aligned for its pixels. */
int pix_solid (const unsigned char *p, int w, int h, int cbpp, int *color);

#endif
