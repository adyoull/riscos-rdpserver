/* RDPServer: pixel formats. See pixels.h.
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <string.h>
#include "pixels.h"

#define MF_RGB_ORDER  (1u << 14)
#define MF_FULL_PAL   (1u << 7)
#define MF_FORMAT     (0xFu << 12)

pix_format
pix_format_of (int log2bpp, int ncolour, unsigned modeflags)
{
  int rgb = (modeflags & MF_RGB_ORDER) != 0;
  /* Bits 12-13 must say RGB; bit 15 (alpha) we can treat as T. */
  if ((modeflags & (3u << 12)) != 0)
    return PIX_UNSUPPORTED;
  if (log2bpp == 5)
    return rgb ? PIX_32_TRGB : PIX_32_TBGR;
  if (log2bpp == 4)
    {
      if (ncolour == 4095)
        return PIX_UNSUPPORTED;   /* 4:4:4 */
      if ((modeflags & MF_FULL_PAL) && ncolour == 65535)
        return rgb ? PIX_16_565_TRGB : PIX_16_565_TBGR;
      return rgb ? PIX_16_555_TRGB : PIX_16_555_TBGR;
    }
  return PIX_UNSUPPORTED;
}

int
pix_bytes (pix_format f)
{
  switch (f)
    {
    case PIX_32_TBGR: case PIX_32_TRGB: return 4;
    case PIX_UNSUPPORTED: return 0;
    default: return 2;
    }
}

int
pix_client_bytes (int client_bpp)
{
  if (client_bpp <= 8)
    return 1;
  if (client_bpp <= 16)
    return 2;
  return 4;
}

/* One screen pixel to 8-bit r, g, b. */
static void
unpack (pix_format f, const unsigned char *p, unsigned *r, unsigned *g, unsigned *b)
{
  unsigned v, x, y, z;
  switch (f)
    {
    case PIX_32_TBGR:
      *r = p[0]; *g = p[1]; *b = p[2];
      return;
    case PIX_32_TRGB:
      *b = p[0]; *g = p[1]; *r = p[2];
      return;
    case PIX_16_555_TBGR: case PIX_16_555_TRGB:
      v = p[0] | (p[1] << 8);
      x = v & 31; y = (v >> 5) & 31; z = (v >> 10) & 31;
      x = (x << 3) | (x >> 2); y = (y << 3) | (y >> 2); z = (z << 3) | (z >> 2);
      break;
    case PIX_16_565_TBGR: case PIX_16_565_TRGB:
      v = p[0] | (p[1] << 8);
      x = v & 31; y = (v >> 5) & 63; z = (v >> 11) & 31;
      x = (x << 3) | (x >> 2); y = (y << 2) | (y >> 4); z = (z << 3) | (z >> 2);
      break;
    default:
      *r = *g = *b = 0;
      return;
    }
  *g = y;
  if (f == PIX_16_555_TBGR || f == PIX_16_565_TBGR)
    { *r = x; *b = z; }
  else
    { *b = x; *r = z; }
}

void
pix_convert (pix_format f, const unsigned char *src, size_t src_stride,
             int w, int h, int client_bpp, void *dst)
{
  int bpp = pix_bytes (f), x, y;
  unsigned char *d8 = dst;
  uint16_t *d16 = dst;
  uint32_t *d32 = dst;

  for (y = 0; y < h; y++)
    {
      const unsigned char *s = src + (size_t) y * src_stride;
      for (x = 0; x < w; x++, s += bpp)
        {
          unsigned r, g, b;
          unpack (f, s, &r, &g, &b);
          switch (client_bpp)
            {
            case 8:
              *d8++ = (unsigned char) ((r & 0xe0) | ((g & 0xe0) >> 3) | (b >> 6));
              break;
            case 15:
              *d16++ = (uint16_t) (((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
              break;
            case 16:
              *d16++ = (uint16_t) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
              break;
            default:
              *d32++ = (r << 16) | (g << 8) | b;
              break;
            }
        }
    }
}

void
pix_palette_332 (int palette[256])
{
  int i;
  for (i = 0; i < 256; i++)
    {
      unsigned r = (i >> 5) & 7, g = (i >> 2) & 7, b = i & 3;
      r = r * 255 / 7;
      g = g * 255 / 7;
      b = b * 255 / 3;
      palette[i] = (int) ((r << 16) | (g << 8) | b);
    }
}
