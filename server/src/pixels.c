/* RDPServer: pixel formats. See pixels.h.
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <string.h>
#include "pixels.h"
#include "platform.h"

#define MF_RGB_ORDER  (1u << 14)
#define MF_FULL_PAL   (1u << 7)
#define MF_FORMAT     (0xFu << 12)

pix_format
pix_format_of (int log2bpp, int ncolour, unsigned modeflags)
{
  int rgb = (modeflags & MF_RGB_ORDER) != 0;
  /* Bits 12-13 must say RGB (not YCbCr); bit 15 (alpha) we treat as T. */
  if ((modeflags & (3u << 12)) != 0)
    return PIX_UNSUPPORTED;
  switch (log2bpp)
    {
    case 0: return PIX_1_PAL;
    case 1: return PIX_2_PAL;
    case 2: return PIX_4_PAL;
    case 3: return PIX_8_PAL;
    case 4:
      if (ncolour == 4095)
        return rgb ? PIX_16_444_TRGB : PIX_16_444_TBGR;
      if ((modeflags & MF_FULL_PAL) && ncolour == 65535)
        return rgb ? PIX_16_565_TRGB : PIX_16_565_TBGR;
      return rgb ? PIX_16_555_TRGB : PIX_16_555_TBGR;
    case 5:
      return rgb ? PIX_32_TRGB : PIX_32_TBGR;
    default:
      return PIX_UNSUPPORTED;
    }
}

int
pix_bits (pix_format f)
{
  switch (f)
    {
    case PIX_32_TBGR: case PIX_32_TRGB: return 32;
    case PIX_8_PAL: return 8;
    case PIX_4_PAL: return 4;
    case PIX_2_PAL: return 2;
    case PIX_1_PAL: return 1;
    case PIX_UNSUPPORTED: return 0;
    default: return 16;
    }
}

int
pix_bytes (pix_format f)
{
  return pix_bits (f) / 8;
}

int
pix_is_palette (pix_format f)
{
  return f == PIX_8_PAL || f == PIX_4_PAL || f == PIX_2_PAL || f == PIX_1_PAL;
}

static uint32_t palette[256];

void
pix_set_palette (const uint32_t *rgb, int n)
{
  int i;
  for (i = 0; i < 256; i++)
    palette[i] = i < n ? rgb[i] & 0xffffff : 0;
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

/* One screen pixel (16 or 32 bpp) to 8-bit r, g, b. */
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
    case PIX_16_444_TBGR: case PIX_16_444_TRGB:
      v = p[0] | (p[1] << 8);
      x = (v & 15) * 17; y = ((v >> 4) & 15) * 17; z = ((v >> 8) & 15) * 17;
      break;
    default:
      *r = *g = *b = 0;
      return;
    }
  *g = y;
  if (f == PIX_16_555_TBGR || f == PIX_16_565_TBGR || f == PIX_16_444_TBGR)
    { *r = x; *b = z; }
  else
    { *b = x; *r = z; }
}

/* 8-bit r, g, b in the client's depth. */
static uint32_t
pack (unsigned r, unsigned g, unsigned b, int client_bpp)
{
  switch (client_bpp)
    {
    case 8:
      return (r & 0xe0) | ((g & 0xe0) >> 3) | (b >> 6);
    case 15:
      return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
    case 16:
      return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    default:
      return (r << 16) | (g << 8) | b;
    }
}

void
pix_convert (pix_format f, const unsigned char *src, size_t src_stride,
             int w, int h, int client_bpp, void *dst)
{
  int bpp = pix_bytes (f), cb = pix_client_bytes (client_bpp), x, y;
  unsigned char *d8 = dst;
  uint16_t *d16 = dst;
  uint32_t *d32 = dst;

  if (pix_is_palette (f))
    {
      /* each colour number once, then a table lookup a pixel */
      int bits = pix_bits (f), n = 1 << bits, i;
      unsigned mask = (unsigned) n - 1;
      uint32_t out[256];
      for (i = 0; i < n; i++)
        out[i] = pack ((palette[i] >> 16) & 255, (palette[i] >> 8) & 255,
                       palette[i] & 255, client_bpp);
      for (y = 0; y < h; y++)
        {
          const unsigned char *s = src + (size_t) y * src_stride;
          for (x = 0; x < w; x++)
            {
              unsigned at = (unsigned) x * (unsigned) bits;
              uint32_t v = out[(s[at >> 3] >> (at & 7)) & mask];
              if (cb == 1) *d8++ = (unsigned char) v;
              else if (cb == 2) *d16++ = (uint16_t) v;
              else *d32++ = v;
            }
        }
      return;
    }
  for (y = 0; y < h; y++)
    {
      const unsigned char *s = src + (size_t) y * src_stride;
      for (x = 0; x < w; x++, s += bpp)
        {
          unsigned r, g, b;
          uint32_t v;
          unpack (f, s, &r, &g, &b);
          v = pack (r, g, b, client_bpp);
          if (cb == 1) *d8++ = (unsigned char) v;
          else if (cb == 2) *d16++ = (uint16_t) v;
          else *d32++ = v;
        }
    }
}

int
pix_convert_fast (pix_format f, const unsigned char *src, size_t src_stride,
                  int w, int h, int client_bpp, void *dst)
{
  int x, y, rgb;
  if (f != PIX_32_TBGR && f != PIX_32_TRGB)
    return -1;
  if (client_bpp != 16 && client_bpp != 24 && client_bpp != 32)
    return -1;
  rgb = f == PIX_32_TRGB;
  for (y = 0; y < h; y++)
    {
      const uint32_t *s = (const uint32_t *) (const void *) (src + (size_t) y * src_stride);
      if (client_bpp == 16)
        {
          uint16_t *d = (uint16_t *) dst + (size_t) y * (size_t) w;
          for (x = 0; x < w; x++)
            {
              uint32_t v = s[x];
              uint32_t r = rgb ? (v >> 16) & 255 : v & 255, g = (v >> 8) & 255,
                       b = rgb ? v & 255 : (v >> 16) & 255;
              d[x] = (uint16_t) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
        }
      else
        {
          uint32_t *d = (uint32_t *) dst + (size_t) y * (size_t) w;
          if (rgb)
            for (x = 0; x < w; x++)
              d[x] = s[x] & 0xffffff;
          else
            for (x = 0; x < w; x++)
              {
                uint32_t v = s[x];
                d[x] = ((v & 255) << 16) | (v & 0xff00) | ((v >> 16) & 255);
              }
        }
    }
  return 0;
}

int pix_use_neon, pix_use_fast;

void
pix_convert_best (pix_format f, const unsigned char *src, size_t src_stride,
                  int w, int h, int client_bpp, void *dst)
{
  int thirty2 = f == PIX_32_TBGR || f == PIX_32_TRGB;
  int to = client_bpp == 16 || client_bpp == 24 || client_bpp == 32;
  if (pix_use_neon && thirty2 && to && w >= 8 && plat_neon_enter () == 0)
    {
      int cb = pix_client_bytes (client_bpp), y, n8 = w & ~7;
      for (y = 0; y < h; y++)
        {
          const unsigned char *s = src + (size_t) y * src_stride;
          unsigned char *d = (unsigned char *) dst + (size_t) y * (size_t) w * (size_t) cb;
          plat_neon_convert (f == PIX_32_TRGB, client_bpp == 16 ? 16 : 32, d, s, (unsigned) n8);
          if (n8 < w)
            pix_convert_fast (f, s + (size_t) n8 * 4, src_stride, w - n8, 1, client_bpp,
                              d + (size_t) n8 * (size_t) cb);
        }
      plat_neon_leave ();
      return;
    }
  if (pix_use_fast && pix_convert_fast (f, src, src_stride, w, h, client_bpp, dst) == 0)
    return;
  pix_convert (f, src, src_stride, w, h, client_bpp, dst);
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

/* Whether the w x h converted pixels (packed, in the client's depth) are
   all one colour; if so, *color is it as an OpaqueRect order takes it:
   the 8-bit index, the 15/16-bit value, or for 24/32 bpp red, green, blue
   from the lowest byte up (as xrdp's COLOR24BGR; bitmaps hold 0x00RRGGBB). */
int
pix_solid (const unsigned char *p, int w, int h, int cbpp, int *color)
{
  size_t n = (size_t) w * (size_t) h, i;
  if (n == 0)
    return 0;
  if (cbpp <= 8)
    {
      for (i = 1; i < n; i++)
        if (p[i] != p[0])
          return 0;
      *color = p[0];
    }
  else if (cbpp <= 16)
    {
      const uint16_t *q = (const uint16_t *) p;
      for (i = 1; i < n; i++)
        if (q[i] != q[0])
          return 0;
      *color = q[0];
    }
  else
    {
      const uint32_t *q = (const uint32_t *) p;
      uint32_t c;
      for (i = 1; i < n; i++)
        if ((q[i] ^ q[0]) & 0xFFFFFF)
          return 0;
      c = q[0];
      *color = (int) (((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16));
    }
  return 1;
}
