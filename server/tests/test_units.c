/* RDPServer: unit tests for the portable parts (pixels, capture, keymap,
   the crypto in ssl_calls.c, the key's signature). Run on Linux, and as
   ARM code under qemu-arm-aligntrap (RISC OS's alignment rules).

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "arch.h"
#include "os_calls.h"
#include "ssl_calls.h"
#include "pixels.h"
#include "capture.h"
#include "pointer.h"
#include "login.h"
#include "kbdlayout.h"
#include "keymap.h"
#include "keygen.h"
#include "input.h"
#include "auth.h"

static int checks, fails;
#define CHECK(c, what) do { checks++; if (!(c)) { fails++; \
  fprintf (stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

static void test_pixels (void)
{
  /* One pixel each way: pure red, then 0x123456-ish. */
  unsigned char tbgr[8] = { 0xff, 0, 0, 0,  0x12, 0x34, 0x56, 0 };
  unsigned char trgb[8] = { 0, 0, 0xff, 0,  0x56, 0x34, 0x12, 0 };
  uint32_t out32[2];
  uint16_t out16[2];
  unsigned char out8[2];
  uint16_t s565 = 0x001f;           /* TBGR 5:6:5: red in the low bits */
  uint16_t s555 = (uint16_t) (31 << 10);   /* TBGR 1:5:5:5: blue in the top */
  int pal[256];

  CHECK (pix_format_of (5, -1, 0x40) == PIX_32_TBGR, "32 bpp, ModeFlags &40: TBGR");
  CHECK (pix_format_of (5, -1, 0x4000) == PIX_32_TRGB, "bit 14: TRGB");
  CHECK (pix_format_of (4, 65535, 0x80) == PIX_16_565_TBGR, "16 bpp, bit 7, NColour 65535: 5:6:5");
  CHECK (pix_format_of (4, 65535, 0) == PIX_16_555_TBGR, "16 bpp without bit 7: 1:5:5:5");
  CHECK (pix_format_of (3, 255, 0x80) == PIX_8_PAL && pix_format_of (3, 63, 0) == PIX_8_PAL,
         "256 colours (full palette, or the old one): through the palette");
  CHECK (pix_format_of (2, 15, 0) == PIX_4_PAL && pix_format_of (1, 3, 0) == PIX_2_PAL
         && pix_format_of (0, 1, 0) == PIX_1_PAL, "16, 4 and 2 colours");
  CHECK (pix_format_of (4, 4095, 0x80) == PIX_16_444_TBGR
         && pix_format_of (4, 4095, 0x4080) == PIX_16_444_TRGB, "4096 colours: 4:4:4");
  CHECK (pix_format_of (4, 65535, 0x4000) == PIX_16_555_TRGB
         && pix_format_of (4, 65535, 0x4080) == PIX_16_565_TRGB
         && pix_format_of (5, -1, 0xC000) == PIX_32_TRGB
         && pix_format_of (4, 65535, 0x8000) == PIX_16_555_TBGR, "RGB order, and alpha");
  CHECK (pix_bits (PIX_1_PAL) == 1 && pix_bits (PIX_4_PAL) == 4 && pix_bits (PIX_16_444_TBGR) == 16
         && pix_bits (PIX_32_TRGB) == 32 && pix_bits (PIX_UNSUPPORTED) == 0, "bits per pixel");
  CHECK (pix_format_of (6, -1, 0) == PIX_UNSUPPORTED, "64 bpp isn't a RISC OS mode");
  CHECK (pix_format_of (5, -1, 0x2000) == PIX_UNSUPPORTED, "YCbCr isn't shared");

  pix_convert (PIX_32_TBGR, tbgr, 8, 2, 1, 32, out32);
  CHECK (out32[0] == 0xff0000 && out32[1] == 0x123456, "TBGR to 0x00RRGGBB");
  pix_convert (PIX_32_TRGB, trgb, 8, 2, 1, 24, out32);
  CHECK (out32[0] == 0xff0000 && out32[1] == 0x123456, "TRGB to 0x00RRGGBB");
  pix_convert (PIX_32_TBGR, tbgr, 8, 2, 1, 16, out16);
  CHECK (out16[0] == 0xf800, "red to RGB 5:6:5");
  CHECK (out16[1] == (((0x12 >> 3) << 11) | ((0x34 >> 2) << 5) | (0x56 >> 3)), "a colour to 5:6:5");
  pix_convert (PIX_32_TBGR, tbgr, 8, 1, 1, 15, out16);
  CHECK (out16[0] == 0x7c00, "red to RGB 5:5:5");
  pix_convert (PIX_32_TBGR, tbgr, 8, 1, 1, 8, out8);
  CHECK (out8[0] == 0xe0, "red to 3:3:2");
  pix_convert (PIX_16_565_TBGR, (unsigned char *) &s565, 2, 1, 1, 32, out32);
  CHECK (out32[0] == 0xff0000, "5:6:5 TBGR low bits are red");
  pix_convert (PIX_16_555_TBGR, (unsigned char *) &s555, 2, 1, 1, 32, out32);
  CHECK (out32[0] == 0x0000ff, "1:5:5:5 TBGR top bits are blue");
  {
    /* 4:4:4 TBGR: red 0xf, green 0x8, blue 0x1 */
    uint16_t s444 = 0x18f, t444 = 0xf81;
    pix_convert (PIX_16_444_TBGR, (unsigned char *) &s444, 2, 1, 1, 32, out32);
    CHECK (out32[0] == 0xff8811, "4:4:4 TBGR to 0x00RRGGBB");
    pix_convert (PIX_16_444_TRGB, (unsigned char *) &t444, 2, 1, 1, 32, out32);
    CHECK (out32[0] == 0xff8811, "4:4:4 TRGB");
  }
  {
    /* the palette: 4 bpp, 16 bpp and 8 bpp clients */
    static const uint32_t pal16[16] = { 0x000000, 0xff0000, 0x00ff00, 0x0000ff, 0x123456 };
    unsigned char p4[2] = { 0x21, 0x43 }, p1 = 0xA5, p2 = 0xE4, p8[2] = { 3, 1 };
    pix_set_palette (pal16, 16);
    pix_convert (PIX_4_PAL, p4, 2, 4, 1, 32, out32);
    CHECK (out32[0] == 0xff0000 && out32[1] == 0x00ff00 && out32[2] == 0x0000ff
           && out32[3] == 0x123456, "4 bpp: two pixels a byte, the left one in the low bits");
    pix_convert (PIX_4_PAL, p4, 2, 2, 1, 16, out16);
    CHECK (out16[0] == 0xf800 && out16[1] == 0x07e0, "4 bpp to RGB 5:6:5");
    pix_convert (PIX_4_PAL, p4, 2, 1, 1, 8, out8);
    CHECK (out8[0] == 0xe0, "4 bpp to the 3:3:2 palette");
    pix_convert (PIX_8_PAL, p8, 2, 2, 1, 24, out32);
    CHECK (out32[0] == 0x0000ff && out32[1] == 0xff0000, "8 bpp");
    pix_convert (PIX_1_PAL, &p1, 1, 8, 1, 32, out32);
    CHECK (out32[0] == 0xff0000 && out32[1] == 0 && out32[2] == 0xff0000 && out32[7] == 0xff0000
           && out32[6] == 0, "1 bpp: eight pixels a byte, bit 0 first");
    pix_convert (PIX_2_PAL, &p2, 1, 4, 1, 32, out32);
    CHECK (out32[0] == 0 && out32[1] == 0xff0000 && out32[2] == 0x00ff00 && out32[3] == 0x0000ff,
           "2 bpp: four pixels a byte");
    {
      /* rows: the stride, not the width */
      unsigned char rows[2][4] = { { 0x11, 0, 0, 0 }, { 0x22, 0, 0, 0 } };
      pix_convert (PIX_4_PAL, &rows[0][0], 4, 2, 2, 32, out32);
      CHECK (out32[0] == 0xff0000 && out32[2] == 0x00ff00 && out32[3] == 0x00ff00, "palette rows by the stride");
    }
    pix_set_palette (pal16, 2);
    pix_convert (PIX_4_PAL, p4, 2, 2, 1, 32, out32);
    CHECK (out32[0] == 0xff0000 && out32[1] == 0, "entries past those given are black");
  }
  pix_palette_332 (pal);
  CHECK (pal[0xe0] == 0xff0000 && pal[0x1c] == 0x00ff00 && pal[0x03] == 0x0000ff
         && pal[0xff] == 0xffffff && pal[0] == 0, "the 3:3:2 palette's corners");
}

/* The restarting page (0.2-test24): background, panel and its text. */
static void test_notice (void)
{
  login_ui l;
  int x, y, dark = 0;
  CHECK (login_notice (&l, 640, 480, "RISC OS is restarting", "Connect again once it's back") == 0,
         "notice: drawn");
  CHECK (l.px[10 * 640 + 10] == 0x45332a, "notice: the background in the corner");
  CHECK (l.px[278 * 640 + 590] == 0xeeeeee, "notice: the panel inside it");
  for (y = l.py0; y < l.py0 + l.ph; y++)
    for (x = l.px0; x < l.px0 + l.pw; x++)
      if (l.px[y * 640 + x] == 0x101010)
        dark++;
  CHECK (dark > 500, "notice: text on the panel");
  login_close (&l);
  CHECK (login_notice (&l, 32, 32, "x", "y") == -1, "notice: too small a screen");
}

/* pix_solid: one colour or not, and the colour as OpaqueRect takes it */
static void test_solid (void)
{
  static uint32_t b32[64 * 64];
  static uint16_t b16[64 * 64];
  static unsigned char b8[64 * 64];
  int c = -1, i;
  for (i = 0; i < 64 * 64; i++)
    {
      b32[i] = 0x00123456;
      b16[i] = 0xF81F;
      b8[i] = 0xE0;
    }
  CHECK (pix_solid ((unsigned char *) b32, 64, 64, 32, &c) && c == 0x563412,
         "32 bpp solid: red, green, blue from the lowest byte (0x00RRGGBB 123456)");
  CHECK (pix_solid ((unsigned char *) b32, 64, 64, 24, &c) && c == 0x563412, "24 bpp likewise");
  b32[4095] = 0xFF123456;
  CHECK (pix_solid ((unsigned char *) b32, 64, 64, 32, &c), "32 bpp: the top byte isn't colour");
  b32[4095] = 0x00123457;
  CHECK (!pix_solid ((unsigned char *) b32, 64, 64, 32, &c), "32 bpp: the last pixel differs");
  CHECK (pix_solid ((unsigned char *) b32, 64, 63, 32, &c), "but not in the rows asked about");
  CHECK (pix_solid ((unsigned char *) b16, 64, 64, 16, &c) && c == 0xF81F, "16 bpp: the value");
  CHECK (pix_solid ((unsigned char *) b16, 64, 64, 15, &c) && c == 0xF81F, "15 bpp likewise");
  b16[100] = 0;
  CHECK (!pix_solid ((unsigned char *) b16, 64, 64, 16, &c), "16 bpp: one differs");
  CHECK (pix_solid (b8, 64, 64, 8, &c) && c == 0xE0, "8 bpp: the index");
  b8[1] = 0;
  CHECK (!pix_solid (b8, 64, 64, 8, &c), "8 bpp: one differs");
  CHECK (pix_solid (b8, 1, 1, 8, &c) && c == 0xE0, "one pixel is solid");
  CHECK (!pix_solid (b8, 0, 5, 8, &c), "nothing isn't");
}

static void test_capture (void)
{
  enum { W = 150, H = 70, BPP = 4 };   /* partial tiles both ways; 32 bpp */
  size_t stride = W * BPP + 12;
  unsigned char *scr = calloc (H, stride);
  capture c;
  int i, x, y, w, h, n;

  CHECK (cap_init (&c, W, H, BPP * 8) == 0, "cap_init");
  CHECK (cap_init (&c, W, H, 12) != 0 && cap_init (&c, W, H, 0) != 0, "only 1, 2, 4, 8, 16, 32 bits");
  CHECK (cap_init (&c, W, H, BPP * 8) == 0, "cap_init again");
  CHECK (c.tiles_x == 3 && c.tiles_y == 2, "3 x 2 tiles");
  /* Everything starts dirty. */
  i = 0; n = 0;
  while (cap_next_dirty (&c, &i, &x, &y, &w, &h))
    n++;
  CHECK (n == 6, "first frame: all 6 tiles");
  i = 0;
  CHECK (!cap_next_dirty (&c, &i, &x, &y, &w, &h), "and then none");

  /* A full scan of a black screen against a black copy: nothing. */
  CHECK (cap_scan (&c, scr, stride, H) == 0, "no change, no tiles");
  /* One pixel in the bottom-right partial tile. */
  scr[69 * stride + 149 * BPP] = 1;
  CHECK (cap_scan (&c, scr, stride, H) == 1, "one change, one tile");
  i = 0;
  CHECK (cap_next_dirty (&c, &i, &x, &y, &w, &h) && x == 149 && y == 69 && w == 1 && h == 1,
         "only the changed pixel is sent");
  CHECK (cap_pixel (&c, 149, 69)[0] == 1, "the copy has the new pixel");
  CHECK (cap_scan (&c, scr, stride, H) == 0, "scanned again: no change");

  /* Scanning in bands wraps round. */
  scr[10 * stride] = 7;
  CHECK (cap_scan (&c, scr, stride, 5) == 0, "rows 0-4 don't include row 10");
  CHECK (cap_scan (&c, scr, stride, 10) == 1, "rows 5-14 do");
  CHECK (c.next_row == 15, "the next band starts at row 15");
  cap_scan (&c, scr, stride, 60);
  CHECK (c.next_row == 5, "and wraps at the bottom");

  /* A redraw request marks the tiles under it, clipped. */
  i = 0;
  while (cap_next_dirty (&c, &i, &x, &y, &w, &h))
    ;
  cap_mark (&c, 60, -5, 10, 10);
  i = 0; n = 0;
  while (cap_next_dirty (&c, &i, &x, &y, &w, &h))
    n++;
  CHECK (n == 2, "60..69 x 0..4 covers tiles 0 and 1 of the top row");
  cap_mark (&c, 500, 500, 10, 10);
  i = 0;
  CHECK (!cap_next_dirty (&c, &i, &x, &y, &w, &h), "off the screen: nothing");

  /* The rectangle within a tile: the box round what changed. */
  scr[3 * stride + 70 * BPP] ^= 0x40;     /* tile 1: (70,3) */
  scr[20 * stride + 100 * BPP + 2] ^= 0x40; /* tile 1: (100,20), a high byte */
  scr[40 * stride + 66 * BPP + 3] ^= 0x40;  /* tile 1: (66,40), the top byte */
  cap_scan (&c, scr, stride, H);
  i = 0;
  CHECK (cap_next_dirty (&c, &i, &x, &y, &w, &h) && x == 66 && y == 3 && w == 35 && h == 38,
         "three changes in one tile: their union");
  CHECK (!cap_next_dirty (&c, &i, &x, &y, &w, &h), "and only that tile");
  /* the box starts again once sent */
  scr[30 * stride + 90 * BPP] ^= 0x40;
  cap_scan (&c, scr, stride, H);
  i = 0;
  CHECK (cap_next_dirty (&c, &i, &x, &y, &w, &h) && x == 90 && y == 30 && w == 1 && h == 1,
         "sent: the next change starts a new box");
  /* a change, then a redraw over part of the same tile */
  scr[10 * stride + 5 * BPP] ^= 0x40;
  cap_scan (&c, scr, stride, H);
  cap_mark (&c, 30, 50, 100, 100);
  i = 0; n = 0;
  CHECK (cap_next_dirty (&c, &i, &x, &y, &w, &h) && x == 5 && y == 10 && w == 59 && h == 54,
         "a change and a redraw: tile 0 sends both");
  CHECK (cap_next_dirty (&c, &i, &x, &y, &w, &h) && x == 64 && y == 50 && w == 64 && h == 14,
         "the redraw, clipped to tile 1");
  CHECK (cap_next_dirty (&c, &i, &x, &y, &w, &h) && x == 128 && y == 50 && w == 2 && h == 14,
         "and to the end of the mark in tile 2");
  CHECK (cap_next_dirty (&c, &i, &x, &y, &w, &h) && x == 30 && y == 64 && w == 34 && h == 6,
         "the bottom row, clipped to the screen");
  i = 0;
  while (cap_next_dirty (&c, &i, &x, &y, &w, &h))
    ;
  /* everything: whole tiles, clipped at the edges */
  cap_mark_all (&c);
  i = 0; n = 0;
  while (cap_next_dirty (&c, &i, &x, &y, &w, &h))
    {
      int fw = x + 64 <= W ? 64 : W - x, fh = y + 64 <= H ? 64 : H - y;
      if (x % 64 == 0 && y % 64 == 0 && w == fw && h == fh)
        n++;
    }
  CHECK (n == 6, "cap_mark_all: every tile, whole");
  cap_free (&c);
  free (scr);

  /* 4 bpp: two pixels a byte; rectangles widened to whole bytes */
  {
    enum { W4 = 150, H4 = 70 };
    size_t st4 = W4 / 2 + 8;
    unsigned char *s4 = calloc (H4, st4);
    CHECK (cap_init (&c, W4, H4, 4) == 0 && c.row_bytes == W4 / 2, "4 bpp: 75 bytes a row");
    i = 0;
    while (cap_next_dirty (&c, &i, &x, &y, &w, &h))
      ;
    s4[10 * st4 + 70 / 2] = 0x10;            /* pixel 71 (the high half of byte 35) */
    CHECK (cap_scan (&c, s4, st4, H4) == 1, "4 bpp: one tile");
    i = 0;
    CHECK (cap_next_dirty (&c, &i, &x, &y, &w, &h) && x == 70 && y == 10 && w == 2 && h == 1,
           "4 bpp: the whole byte's two pixels");
    CHECK (cap_pixel (&c, 70, 10) == c.frame + 10 * c.row_bytes + 35, "4 bpp: cap_pixel's byte");
    s4[69 * st4 + 74] = 0x01;                /* pixel 148, the last byte */
    cap_scan (&c, s4, st4, H4);
    i = 0;
    CHECK (cap_next_dirty (&c, &i, &x, &y, &w, &h) && x == 148 && w == 2, "4 bpp: the last byte");
    cap_mark (&c, 65, 1, 2, 2);
    i = 0;
    CHECK (cap_next_dirty (&c, &i, &x, &y, &w, &h) && x == 64 && w == 4, "4 bpp: a mark widened to bytes");
    cap_free (&c);
    free (s4);
  }
  /* 1 bpp: eight pixels a byte */
  {
    unsigned char s1[64 * 16];
    memset (s1, 0, sizeof s1);
    CHECK (cap_init (&c, 128, 64, 1) == 0 && c.row_bytes == 16, "1 bpp: 16 bytes a row");
    i = 0;
    while (cap_next_dirty (&c, &i, &x, &y, &w, &h))
      ;
    s1[5 * 16 + 9] = 0x80;                   /* pixel 79 */
    cap_scan (&c, s1, 16, 64);
    i = 0;
    CHECK (cap_next_dirty (&c, &i, &x, &y, &w, &h) && x == 72 && w == 8 && y == 5,
           "1 bpp: the byte's eight pixels");
    cap_free (&c);
  }
}

/* cap_scan_sampled: 1 row in c.sample a pass, the rest of a tile where
   one changed, and every row once in c.sample passes. */
static int frame_matches (const capture *c, const unsigned char *scr, size_t stride)
{
  int y;
  for (y = 0; y < c->height; y++)
    if (memcmp (c->frame + (size_t) y * c->row_bytes, scr + (size_t) y * stride, c->row_bytes))
      return 0;
  return 1;
}

static void drain (capture *c)
{
  int i = 0, x, y, w, h;
  while (cap_next_dirty (c, &i, &x, &y, &w, &h))
    ;
}

static void test_capture_sampled (void)
{
  enum { W = 300, H = 200, BPP = 4 };
  size_t stride = W * BPP + 4;
  unsigned char *scr = calloc (H, stride);
  capture c;
  int i, x, y, w, h, p, found_at, bits, sample;
  unsigned long long r0;
  static const int order8[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

  for (y = 0; y < H; y++)
    for (x = 0; x < W * BPP; x++)
      scr[y * stride + x] = (unsigned char) (x * 7 + y * 13);
  CHECK (cap_init (&c, W, H, 32) == 0 && c.sample == 1, "sampled: cap_init, sample 1");
  cap_scan (&c, scr, stride, H);
  drain (&c);
  CHECK (frame_matches (&c, scr, stride), "sampled: the full scan copied the screen");
  c.sample = 8;

  /* a pass with nothing changed reads 25 rows of 200 */
  r0 = c.bytes_read;
  CHECK (cap_scan_sampled (&c, scr, stride, H) == 0 && c.next_row == 0 && c.phase == 1,
         "sampled: a pass, nothing changed, the next phase");
  CHECK (c.bytes_read - r0 == 25 * c.row_bytes, "sampled: 1 row in 8 read");
  /* the order: rows 0, 4, 2, 6, 1, 5, 3, 7 of each 8 */
  {
    int ok = 1;
    for (p = 0; p < 8; p++)
      {
        /* a 1-row change in the row read in pass p (of the next 8) */
        int row = 8 * 3 + order8[(1 + p) % 8];
        scr[row * stride + 40] ^= 0xFF;
        for (found_at = -1, i = 0; i < 8 && found_at < 0; i++)
          if (cap_scan_sampled (&c, scr, stride, H) > 0)
            found_at = i;
        if (found_at != 0)
          ok = 0;
        drain (&c);
      }
    CHECK (ok, "sampled: rows read in the order 0 4 2 6 1 5 3 7");
  }
  /* a 1-row change in a row not read this pass is found within 8 */
  {
    int row = 77, missed = 0;
    while ((row & 7) == order8[c.phase])
      row++;
    scr[row * stride + 80 * BPP] ^= 0x55;
    found_at = -1;
    for (i = 0; i < 8; i++)
      if (cap_scan_sampled (&c, scr, stride, H) > 0)
        {
          found_at = i;
          break;
        }
      else
        missed++;
    CHECK (found_at > 0 && found_at < 8, "sampled: a 1-row change missed at first, found within 8 passes");
    i = 0;
    CHECK (cap_next_dirty (&c, &i, &x, &y, &w, &h) && x == 80 && y == row && w == 1 && h == 1,
           "sampled: and only its pixel sent");
    drain (&c);
    while (c.next_row != 0)
      cap_scan_sampled (&c, scr, stride, 1);
  }
  /* an 8-row change is found in the pass it happens in, and the rest of
     its tile with it (a pixel at row 70, which this pass doesn't read) */
  {
    int ok, py = 70;
    for (y = 100; y < 108; y++)
      for (x = 10; x <= 20; x++)
        scr[y * stride + x * BPP] ^= 0x33;
    if ((py & 7) == order8[c.phase])
      py = 71;
    scr[py * stride + 5 * BPP + 1] ^= 0x11;
    r0 = c.bytes_read;
    ok = cap_scan_sampled (&c, scr, stride, H) == 1;
    CHECK (ok, "sampled: an 8-row change found in one pass, one tile");
    i = 0;
    CHECK (cap_next_dirty (&c, &i, &x, &y, &w, &h) && x == 5 && y == py && w == 16
           && y + h == 108, "sampled: the box has the pixel the pass didn't read");
    CHECK (c.bytes_read - r0 == 25 * c.row_bytes + 63 * 64 * BPP,
           "sampled: the rest of the one tile read, nothing more");
    CHECK (frame_matches (&c, scr, stride), "sampled: the copy is the screen again");
    drain (&c);
  }
  /* bands across calls: the phase moves on only at the bottom */
  {
    int ph = c.phase;
    cap_scan_sampled (&c, scr, stride, 150);
    CHECK (c.phase == ph && c.next_row == 150, "sampled: part of a pass, same phase");
    cap_scan_sampled (&c, scr, stride, 50);
    CHECK (c.phase == (ph + 1) % 8 && c.next_row == 0, "sampled: the bottom, the next phase");
  }
  /* sample 3 isn't a power of two: every row */
  c.sample = 3;
  r0 = c.bytes_read;
  cap_scan_sampled (&c, scr, stride, H);
  CHECK (c.bytes_read - r0 == (unsigned long long) H * c.row_bytes, "sampled: 3 reads every row");
  cap_free (&c);
  free (scr);

  /* random rectangles: after `sample` passes the copy is the screen, at
     every depth and sample, and every changed tile was reported */
  for (bits = 1; bits <= 32; bits *= 2)
    for (sample = 1; sample <= 16; sample *= 2)
      {
        enum { RW = 333, RH = 150 };
        size_t rb = ((size_t) RW * bits + 7) / 8, st = (rb + 7) & ~(size_t) 3;
        unsigned char *rs = calloc (RH, st), *prev = malloc ((size_t) RH * st);
        int round, ok = 1, reported = 1;
        unsigned seed = 1234u + (unsigned) (bits * 31 + sample);
        if (cap_init (&c, RW, RH, bits) != 0)
          {
            CHECK (0, "sampled: random: cap_init");
            break;
          }
        cap_scan (&c, rs, st, RH);
        drain (&c);
        c.sample = sample;
        for (round = 0; round < 6; round++)
          {
            int k, nrect = 1 + round % 3, ty, tx;
            memcpy (prev, rs, (size_t) RH * st);
            for (k = 0; k < nrect; k++)
              {
                int rx0, ry0, rw, rh, yy;
                size_t bx;
                seed = seed * 1103515245u + 12345u;
                rx0 = (int) ((seed >> 8) % rb);
                seed = seed * 1103515245u + 12345u;
                ry0 = (int) ((seed >> 8) % RH);
                seed = seed * 1103515245u + 12345u;
                rw = 1 + (int) ((seed >> 8) % 40);
                seed = seed * 1103515245u + 12345u;
                rh = 1 + (int) ((seed >> 8) % 20);
                for (yy = ry0; yy < ry0 + rh && yy < RH; yy++)
                  for (bx = (size_t) rx0; bx < (size_t) rx0 + (size_t) rw && bx < rb; bx++)
                    rs[yy * st + bx] ^= (unsigned char) (1 + (seed >> 24) % 255);
              }
            for (p = 0; p < sample; p++)
              cap_scan_sampled (&c, rs, st, RH);
            if (!frame_matches (&c, rs, st))
              ok = 0;
            /* every tile whose bytes differ from before the round is dirty */
            for (ty = 0; ty < c.tiles_y; ty++)
              for (tx = 0; tx < c.tiles_x; tx++)
                {
                  size_t piece = (size_t) 64 * bits / 8, off = (size_t) tx * piece;
                  size_t len = off + piece <= rb ? piece : rb - off;
                  int yy, differs = 0;
                  for (yy = ty * 64; yy < ty * 64 + 64 && yy < RH; yy++)
                    if (memcmp (rs + yy * st + off, prev + yy * st + off, len))
                      differs = 1;
                  if (differs && !c.dirty[ty * c.tiles_x + tx])
                    reported = 0;
                }
            drain (&c);
          }
        free (prev);
        CHECK (ok && reported, "sampled: random changes all found (one depth and sample)");
        cap_free (&c);
        free (rs);
      }
}

static void test_pointer (void)
{
  static unsigned char data[PTR_SIZE * PTR_SIZE * 3], mask[PTR_SIZE * PTR_SIZE / 8];
  int hx = -1, hy = -1, x, y, opaque = 0, white = 0, stray = 0;
#define MASKED(x, y) (mask[(PTR_SIZE - 1 - (y)) * (PTR_SIZE / 8) + (x) / 8] & (0x80 >> ((x) % 8)))
#define PIX(x, y) (data + ((PTR_SIZE - 1 - (y)) * PTR_SIZE + (x)) * 3)
  pointer_arrow (data, mask, &hx, &hy);
  CHECK (hx == 0 && hy == 0, "the hot spot is the arrow's tip");
  CHECK (!MASKED (0, 0) && PIX (0, 0)[0] == 0 && PIX (0, 0)[2] == 0, "the tip is black");
  CHECK (!MASKED (1, 2) && PIX (1, 2)[0] == 255 && PIX (1, 2)[1] == 255, "inside is white (rows bottom first)");
  CHECK (MASKED (20, 0) && MASKED (31, 31) && MASKED (0, 31), "outside is transparent");
  for (y = 0; y < PTR_SIZE; y++)
    for (x = 0; x < PTR_SIZE; x++)
      if (!MASKED (x, y))
        {
          opaque++;
          white += PIX (x, y)[0] == 255;
        }
      else
        stray += PIX (x, y)[0] != 0;
  CHECK (stray == 0, "transparent pixels have no colour");
  CHECK (opaque > 100 && opaque < 250 && white > 40 && white < opaque, "an arrow-sized shape, outlined");
#undef MASKED
#undef PIX
}

#define KC(k, code, sh, ag, want) CHECK (kbd_char (k, code, sh, ag, 0) == (unsigned) (want), \
                                          "keyboard " #k " key " #code " shift " #sh " altgr " #ag)
static void test_kbdlayout (void)
{

 /* USA */ KC(KBD_USA,0x0D,0,0,'=');KC(KBD_USA,0x1B,1,0,'}');KC(KBD_USA,0x29,0,0,'`');KC(KBD_USA,0x2B,1,0,'|');KC(KBD_USA,0x35,1,0,'?');KC(KBD_USA,0x56,0,0,'\\');KC(KBD_USA,0x03,1,0,'@');KC(KBD_USA,0x10,0,0,'q');
 /* UK */ KC(KBD_UK,0x03,1,0,'"');KC(KBD_UK,0x04,1,0,0xA3);KC(KBD_UK,0x28,1,0,'@');KC(KBD_UK,0x29,1,0,0xAC);KC(KBD_UK,0x29,0,1,0xA6);KC(KBD_UK,0x2B,0,0,'#');KC(KBD_UK,0x2B,1,0,'~');KC(KBD_UK,0x56,1,0,'|');KC(KBD_UK,0x35,0,0,'/');KC(KBD_UK,0x0D,1,0,'+');
 /* DE */ KC(KBD_GERMANY,0x0C,0,0,0xDF);KC(KBD_GERMANY,0x0D,0,0,0);KC(KBD_GERMANY,0x15,0,0,'z');KC(KBD_GERMANY,0x2C,0,0,'y');KC(KBD_GERMANY,0x1A,0,0,0xFC);KC(KBD_GERMANY,0x1B,0,0,'+');KC(KBD_GERMANY,0x1B,1,0,'*');KC(KBD_GERMANY,0x1B,0,1,'~');
 KC(KBD_GERMANY,0x27,0,0,0xF6);KC(KBD_GERMANY,0x28,0,0,0xE4);KC(KBD_GERMANY,0x29,1,0,0xB0);KC(KBD_GERMANY,0x2B,1,0,'\'');KC(KBD_GERMANY,0x35,1,0,'_');KC(KBD_GERMANY,0x56,0,1,'|');KC(KBD_GERMANY,0x10,0,1,'@');KC(KBD_GERMANY,0x0C,0,1,'\\');KC(KBD_GERMANY,0x08,0,1,'{');KC(KBD_GERMANY,0x04,1,0,0xA7);KC(KBD_GERMANY,0x32,0,1,0xB5);KC(KBD_GERMANY,0x08,1,0,'/');KC(KBD_GERMANY,0x09,1,0,'(');
 /* FR */ KC(KBD_FRANCE,0x02,0,0,'&');KC(KBD_FRANCE,0x02,1,0,'1');KC(KBD_FRANCE,0x0B,0,1,'@');KC(KBD_FRANCE,0x0D,0,1,'}');KC(KBD_FRANCE,0x0D,1,0,'+');KC(KBD_FRANCE,0x10,0,0,'a');KC(KBD_FRANCE,0x1A,0,0,0);KC(KBD_FRANCE,0x1B,0,0,'$');KC(KBD_FRANCE,0x1B,1,0,0xA3);KC(KBD_FRANCE,0x1B,0,1,0xA4);
 KC(KBD_FRANCE,0x27,0,0,'m');KC(KBD_FRANCE,0x28,0,0,0xF9);KC(KBD_FRANCE,0x29,0,0,0xB2);KC(KBD_FRANCE,0x2B,0,0,'*');KC(KBD_FRANCE,0x2B,1,0,0xB5);KC(KBD_FRANCE,0x35,0,0,'!');KC(KBD_FRANCE,0x35,1,0,0xA7);KC(KBD_FRANCE,0x56,1,0,'>');KC(KBD_FRANCE,0x04,0,1,'#');KC(KBD_FRANCE,0x09,0,1,'\\');
 /* NL */ KC(KBD_NETHERLANDS,0x0C,0,0,'/');KC(KBD_NETHERLANDS,0x0D,1,0,'~');KC(KBD_NETHERLANDS,0x0D,0,1,0xB8);KC(KBD_NETHERLANDS,0x0C,0,1,'\\');KC(KBD_NETHERLANDS,0x0A,0,1,'}');KC(KBD_NETHERLANDS,0x08,1,0,'_');KC(KBD_NETHERLANDS,0x1B,0,0,'*');KC(KBD_NETHERLANDS,0x1B,1,0,'|');
 KC(KBD_NETHERLANDS,0x27,0,0,'+');KC(KBD_NETHERLANDS,0x27,1,0,0xB1);KC(KBD_NETHERLANDS,0x29,0,0,'@');KC(KBD_NETHERLANDS,0x29,1,0,0xA7);KC(KBD_NETHERLANDS,0x29,0,1,0xAC);KC(KBD_NETHERLANDS,0x2B,0,0,'<');KC(KBD_NETHERLANDS,0x35,1,0,'=');KC(KBD_NETHERLANDS,0x56,0,0,']');KC(KBD_NETHERLANDS,0x56,0,1,0xA6);KC(KBD_NETHERLANDS,0x1F,0,1,0xDF);KC(KBD_NETHERLANDS,0x32,0,1,0xB5);
 
 KC(99,0x10,0,0,'q'); KC(KBD_UK,0x39,0,0,' ');

  CHECK (kbd_char (KBD_GERMANY, 0x27, 0, 0, 1) == 0xD6, "Caps Lock: o-umlaut upper case");
  CHECK (kbd_char (KBD_UK, 0x02, 0, 0, 1) == '1', "Caps Lock: digits stay");
  CHECK (kbd_char (KBD_UK, 0x10, 1, 0, 1) == 'q', "Caps Lock and Shift: lower case");
  CHECK (kbd_known (KBD_NETHERLANDS) && !kbd_known (99) && kbd_from_client (0x0407) == KBD_GERMANY
         && kbd_from_client (0x0411) == 0, "which layouts there are");
}

static void test_utf8 (void)
{
  char a[16] = "caf\xc3\xa9!", b[16] = "\xe2\x82\xac5", c[16] = "plain", d[16] = "bad\xc3";
  auth_utf8_to_latin1 (a);
  CHECK (strcmp (a, "caf\xe9!") == 0, "UTF-8 e-acute to Latin-1");
  auth_utf8_to_latin1 (b);
  CHECK (strcmp (b, "\xe2\x82\xac5") == 0, "a euro sign (not Latin-1): left as it was");
  auth_utf8_to_latin1 (c);
  CHECK (strcmp (c, "plain") == 0, "ASCII unchanged");
  auth_utf8_to_latin1 (d);
  CHECK (strcmp (d, "bad\xc3") == 0, "broken UTF-8: left as it was");
}

static void test_login (void)
{
  login_ui l;
  int i, black = 0, panel = 0;
  CHECK (login_open (&l, 640, 480, "test", KBD_UK) == 0, "login_open");
  CHECK (l.field == 1, "a user name given: the password has the focus");
  /* x, Shift-2 ("), Backspace, y */
  login_scancode (&l, 0x2D, 0, 0); login_scancode (&l, 0x2D, 0, 1);
  login_scancode (&l, 0x2A, 0, 0); login_scancode (&l, 0x03, 0, 0); login_scancode (&l, 0x2A, 0, 1);
  CHECK (strcmp (l.pass, "x\"") == 0, "typed, with Shift (UK: Shift-2 is a double quote)");
  login_scancode (&l, 0x0E, 0, 0);
  login_scancode (&l, 0x15, 0, 0);
  CHECK (strcmp (l.pass, "xy") == 0, "Backspace");
  login_scancode (&l, 0x38, 1, 0); login_scancode (&l, 0x29, 0, 0); login_scancode (&l, 0x38, 1, 1);
  CHECK ((unsigned char) l.pass[2] == 0xA6 && strlen (l.pass) == 3, "AltGr (right Alt): Latin-1");
  CHECK (login_scancode (&l, 0x1C, 0, 0) == LOGIN_SUBMIT, "Return in the password: log in");
  CHECK (login_scancode (&l, 0x0F, 0, 0) == 0 && l.field == 0, "Tab: the other field");
  CHECK (login_scancode (&l, 0x1C, 0, 0) == 0 && l.field == 1, "Return in the user name: on to the password");
  CHECK (login_unicode (&l, 'z', 0) == 0 && l.pass[3] == 'z', "a Unicode key event");
  CHECK (login_unicode (&l, 0x20AC, 0) == 0 && strlen (l.pass) == 4, "not Latin-1: ignored");
  CHECK (login_click (&l, l.bx + 2, l.by + 2) == LOGIN_SUBMIT, "the button");
  CHECK (login_click (&l, l.fx + 3, l.fy[0] + 3) == 0 && l.field == 0, "a click in a field: the focus");
  for (i = 0; i < 70; i++)
    login_unicode (&l, 'a', 0);
  CHECK (strlen (l.user) == LOGIN_FIELD - 1, "a field stops at 63 characters");
  login_clear_password (&l);
  CHECK (l.pass[0] == 0 && l.field == 1, "the password cleared, its field focused");
  for (i = 0; i < 640 * 480; i++)
    {
      black += l.px[i] == 0;
      panel += l.px[i] == 0xeeeeee;
    }
  CHECK (black == 0 && panel > 20000, "drawn: background and panel, nothing left black");
  login_close (&l);
  CHECK (l.px == NULL, "closed");
  CHECK (login_open (&l, 320, 256, "", KBD_USA) == 0 && l.scale == 1 && l.field == 0
         && l.px0 >= 0 && l.px0 + l.pw <= 320 && l.py0 + l.ph <= 256, "a small mode: the small font, it fits");
  login_close (&l);
  CHECK (login_open (&l, 32, 32, "", KBD_USA) != 0, "too small: refused");
}

static void test_keymap (void)
{
  int seen[128] = { 0 }, sc, e, dup = 0, mapped = 0;
  CHECK (keymap_lookup (0x01, 0) == 0x00, "Escape");
  CHECK (keymap_lookup (0x1E, 0) == 0x3C, "A");
  CHECK (keymap_lookup (0x2A, 0) == 0x4C, "left Shift");
  CHECK (keymap_lookup (0x1C, 0) == 0x47 && keymap_lookup (0x1C, 1) == 0x67, "Return and keypad Enter");
  CHECK (keymap_lookup (0x1D, 0) == 0x3B && keymap_lookup (0x1D, 1) == 0x61, "both Ctrls");
  CHECK (keymap_lookup (0x48, 0) == 0x38 && keymap_lookup (0x48, 1) == 0x59, "keypad 8 and Up");
  CHECK (keymap_lookup (0x53, 1) == 0x34 && keymap_lookup (0x4F, 1) == 0x35, "Delete and Copy/End");
  CHECK (keymap_lookup (0x56, 0) == 0x4D && keymap_lookup (0x2B, 0) == 0x33, "the two UK keys");
  CHECK (keymap_lookup (0x57, 0) == 0x0B && keymap_lookup (0x58, 0) == 0x0C, "F11 and F12");
  CHECK (keymap_lookup (0x59, 0) == -1 && keymap_lookup (200, 0) == -1, "unknown keys");
  /* No two scancodes press the same RISC OS key (but Pause/Break). */
  for (e = 0; e < 2; e++)
    for (sc = 0; sc < 128; sc++)
      {
        int ll = keymap_lookup (sc, e);
        if (ll < 0 || ll == 0x0F)
          continue;
        mapped++;
        if (seen[ll]++)
          dup++;
      }
  CHECK (dup == 0, "no RISC OS key twice");
  /* Low-level keys &00-&6E (111), less Pause (left out above), the
     pound key &1D and keypad # &25 (Archimedes only): 108. */
  CHECK (mapped == 108, "108 keys mapped");
}

static void test_crypto (void)
{
  /* RC4: the "Key" / "Plaintext" vector (Wikipedia's RC4 article). */
  char data[] = "Plaintext";
  static const unsigned char want[9] = { 0xBB, 0xF3, 0x16, 0xE8, 0xD9, 0x40, 0xAF, 0x0A, 0xD3 };
  void *rc4 = ssl_rc4_info_create ();
  unsigned char a[4] = { 0x05, 0, 0, 0 }, e3[1] = { 3 }, m[1] = { 13 }, r[4];
  unsigned char d5[16];
  void *md5;

  ssl_rc4_set_key (rc4, "Key", 3);
  ssl_rc4_crypt (rc4, data, 9);
  CHECK (memcmp (data, want, 9) == 0, "RC4 test vector");
  ssl_rc4_info_delete (rc4);

  /* 5^3 mod 13 = 8, little-endian in and out. */
  CHECK (ssl_mod_exp ((char *) r, 4, (char *) a, 4, (char *) m, 1, (char *) e3, 1) == 1
         && r[0] == 8 && r[1] == 0 && r[3] == 0, "5^3 mod 13");

  md5 = ssl_md5_info_create ();
  ssl_md5_clear (md5);
  ssl_md5_transform (md5, "abc", 3);
  ssl_md5_complete (md5, (char *) d5);
  CHECK (d5[0] == 0x90 && d5[1] == 0x01 && d5[15] == 0x72, "MD5(abc)");
  ssl_md5_info_delete (md5);
}

static void test_keygen (void)
{
  /* The signed part's header for a 2048-bit key, as xrdp's own test key
     has it (keygen/keygen.c g_testkey2048, bytes 0-31). */
  static const unsigned char hdr[32] =
  {
    0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x1c, 0x01,
    0x52, 0x53, 0x41, 0x31, 0x08, 0x01, 0x00, 0x00,
    0x00, 0x08, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00
  };
  unsigned char e[4] = { 1, 0, 1, 0 }, n[256], part[RDPS_CERT_SIGNED_LEN], sig[64];
  int i;

  for (i = 0; i < 256; i++)
    n[i] = (unsigned char) (i * 7 + 1);
  rdps_cert_signed_part (e, n, part);
  CHECK (memcmp (part, hdr, 32) == 0, "proprietary certificate header");
  CHECK (memcmp (part + 32, e, 4) == 0 && memcmp (part + 36, n, 256) == 0, "exponent and modulus");
  CHECK (part[292] == 0 && part[299] == 0, "8 bytes of padding");
  CHECK (rdps_sign_cert (e, n, sig) == 0, "signs");
  CHECK (rdps_check_cert_sig (e, n, sig) == 0, "and the signature checks with the public exponent");
  n[100] ^= 1;
  CHECK (rdps_check_cert_sig (e, n, sig) != 0, "a changed modulus doesn't");
}

/* --- the TLS certificate (keygen.c rdps_make_tls_cert) ---------------------- */

#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/oid.h"
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static void ini_line (FILE *f, const char *name, const unsigned char *le, int len)
{
  int i;
  fprintf (f, "%s=", name);
  for (i = 0; i < len; i++)
    fprintf (f, "%s0x%2.2x", i ? "," : "", le[i]);
  fputc ('\n', f);
}

static int ut_rng (void *p, unsigned char *b, size_t n) { (void) p; g_random ((char *) b, (int) n); return 0; }

/* a file and a 0 after it, as mbedTLS's PEM parsers want (no FS_IO) */
static unsigned char *ut_slurp (const char *name, size_t *len)
{
  static unsigned char buf[8192];
  FILE *f = fopen (name, "rb");
  size_t n;
  if (!f)
    return NULL;
  n = fread (buf, 1, sizeof buf - 1, f);
  fclose (f);
  buf[n] = 0;
  *len = n + 1;
  return buf;
}

static void test_tls_cert (void)
{
  static const unsigned char e[4] = { 1, 0, 1, 0 };
  unsigned char n[128], d[128];
  char dir[] = "/tmp/rdps-cert-XXXXXX", ini[64], key[64], cert[64];
  mbedtls_x509_crt crt;
  mbedtls_pk_context pk;
  FILE *f;
  time_t now = time (NULL);
  int year = gmtime (&now)->tm_year + 1900;

  if (!mkdtemp (dir))
    return;
  snprintf (ini, sizeof ini, "%s/rsakeys.ini", dir);
  snprintf (key, sizeof key, "%s/key.pem", dir);
  snprintf (cert, sizeof cert, "%s/cert.pem", dir);
  CHECK (rdps_make_tls_cert (ini, key, cert) != 0 && access (key, F_OK) != 0
         && access (cert, F_OK) != 0, "no rsakeys.ini: no certificate, nothing left");
  /* a 1024-bit key (quick, also under QEMU; Mbed TLS makes none smaller),
     written little-endian as rsakeys.ini has it */
  {
    mbedtls_rsa_context rsa;
    mbedtls_mpi mn, md;
    mbedtls_rsa_init (&rsa);
    mbedtls_mpi_init (&mn);
    mbedtls_mpi_init (&md);
    CHECK (mbedtls_rsa_gen_key (&rsa, ut_rng, NULL, 1024, 65537) == 0
           && mbedtls_rsa_export (&rsa, &mn, NULL, NULL, &md, NULL) == 0
           && mbedtls_mpi_write_binary_le (&mn, n, sizeof n) == 0
           && mbedtls_mpi_write_binary_le (&md, d, sizeof d) == 0, "a test key");
    mbedtls_mpi_free (&mn);
    mbedtls_mpi_free (&md);
    mbedtls_rsa_free (&rsa);
  }
  f = fopen (ini, "w");
  fputs ("[keys]\n", f);
  ini_line (f, "pub_exp", e, 4);
  ini_line (f, "pub_mod", n, 128);
  ini_line (f, "pri_exp", d, 128);
  fclose (f);
  CHECK (rdps_make_tls_cert (ini, key, cert) != 0 && access (key, F_OK) != 0,
         "a key under 2048 bits is refused");
  rdps_tls_small_keys_ok = 1;
  CHECK (rdps_make_tls_cert (ini, key, cert) == 0, "key.pem and cert.pem made from rsakeys.ini");
  {
    struct stat st;
    CHECK (stat (key, &st) == 0 && (st.st_mode & 077) == 0, "key.pem readable by its owner only");
  }

  mbedtls_x509_crt_init (&crt);
  mbedtls_pk_init (&pk);
  {
    size_t len = 0;
    unsigned char *b = ut_slurp (cert, &len);
    CHECK (b && mbedtls_x509_crt_parse (&crt, b, len) == 0, "the certificate parses");
    b = ut_slurp (key, &len);
    CHECK (b && mbedtls_pk_parse_key (&pk, b, len, NULL, 0, ut_rng, NULL) == 0, "the key parses");
  }
  CHECK (mbedtls_pk_check_pair (&crt.pk, &pk, ut_rng, NULL) == 0, "and they're a pair");
  CHECK (crt.version == 3 && crt.MBEDTLS_PRIVATE (sig_md) == MBEDTLS_MD_SHA256, "X.509 v3, SHA-256");
  CHECK ((crt.MBEDTLS_PRIVATE (ext_types) & MBEDTLS_X509_EXT_EXTENDED_KEY_USAGE)
         && (crt.MBEDTLS_PRIVATE (ext_types) & MBEDTLS_X509_EXT_KEY_USAGE),
         "it has key usage and extended key usage (none would mean any)");
  CHECK (mbedtls_x509_crt_check_extended_key_usage (&crt, MBEDTLS_OID_SERVER_AUTH,
           MBEDTLS_OID_SIZE (MBEDTLS_OID_SERVER_AUTH)) == 0, "for server authentication");
  CHECK (mbedtls_x509_crt_check_key_usage (&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE
           | MBEDTLS_X509_KU_KEY_ENCIPHERMENT) == 0, "digital signature, key encipherment");
  CHECK (crt.MBEDTLS_PRIVATE (ca_istrue) == 0, "not a CA");
  CHECK (crt.valid_from.year <= year && crt.valid_to.year >= year + 19, "valid from now for 20 years");
  CHECK (crt.subject_raw.len == crt.issuer_raw.len
         && memcmp (crt.subject_raw.p, crt.issuer_raw.p, crt.subject_raw.len) == 0, "self-signed");
  {
    char name[256];
    mbedtls_x509_dn_gets (name, sizeof name, &crt.subject);
    CHECK (strstr (name, "O=RDPServer") && strstr (name, "CN="), "named for RDPServer");
  }
  {
    /* (not mbedtls_x509_crt_verify: its profile refuses a 1024-bit key) */
    unsigned char h[32];
    CHECK (mbedtls_md (mbedtls_md_info_from_type (MBEDTLS_MD_SHA256), crt.tbs.p, crt.tbs.len, h) == 0
           && mbedtls_pk_verify (&crt.pk, MBEDTLS_MD_SHA256, h, 32, crt.MBEDTLS_PRIVATE (sig).p,
                                 crt.MBEDTLS_PRIVATE (sig).len) == 0,
           "its signature checks with its own key");
  }
  mbedtls_x509_crt_free (&crt);
  mbedtls_pk_free (&pk);

  /* a key whose private exponent doesn't fit its modulus */
  d[5] ^= 0x40;
  f = fopen (ini, "w");
  fputs ("[keys]\n", f);
  ini_line (f, "pub_exp", e, 4);
  ini_line (f, "pub_mod", n, 128);
  ini_line (f, "pri_exp", d, 128);
  fclose (f);
  CHECK (rdps_make_tls_cert (ini, key, cert) != 0 && access (key, F_OK) != 0
         && access (cert, F_OK) != 0, "a broken key: refused, nothing left");
  rdps_tls_small_keys_ok = 0;
  remove (ini);
  rmdir (dir);
}

/* --- TLS when the network backs up (ssl_calls.c ssl_tls_write) -----------
   openssl s_client is the client. Once the handshake is done it's stopped
   (SIGSTOP), so the server's writes fill the socket and come back "would
   block", as on a slow link; they're retried with the same data, as trans
   does, and the client, let go again, must get every byte in order. */

#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include "trans.h"
#include "tls.h"

static void test_tls_backpressure (void)
{
  char dir[] = "/tmp/rdps-tlsbp-XXXXXX", ini[64], key[64], cert[64], out[64], port_s[32];
  static const unsigned char e[4] = { 1, 0, 1, 0 };
  unsigned char n[128], d[128];
  static unsigned char data[1 << 21];
  struct sockaddr_in sa;
  socklen_t sl = sizeof sa;
  int lfd, fd, i, would = 0, stopped = 1, status;
  size_t sent = 0;
  pid_t child;
  struct trans t;
  struct ssl_tls *tls;
  FILE *f;

  if (system ("openssl version >/dev/null 2>&1") != 0)
    {
      printf ("(no openssl command: TLS backpressure test skipped)\n");
      return;
    }
  if (!mkdtemp (dir))
    return;
  snprintf (ini, sizeof ini, "%s/rsakeys.ini", dir);
  snprintf (key, sizeof key, "%s/key.pem", dir);
  snprintf (cert, sizeof cert, "%s/cert.pem", dir);
  snprintf (out, sizeof out, "%s/received", dir);
  {
    mbedtls_rsa_context rsa;
    mbedtls_mpi mn, md;
    mbedtls_rsa_init (&rsa);
    mbedtls_mpi_init (&mn);
    mbedtls_mpi_init (&md);
    mbedtls_rsa_gen_key (&rsa, ut_rng, NULL, 1024, 65537);
    mbedtls_rsa_export (&rsa, &mn, NULL, NULL, &md, NULL);
    mbedtls_mpi_write_binary_le (&mn, n, sizeof n);
    mbedtls_mpi_write_binary_le (&md, d, sizeof d);
    mbedtls_mpi_free (&mn);
    mbedtls_mpi_free (&md);
    mbedtls_rsa_free (&rsa);
  }
  f = fopen (ini, "w");
  fputs ("[keys]\n", f);
  ini_line (f, "pub_exp", e, 4);
  ini_line (f, "pub_mod", n, 128);
  ini_line (f, "pri_exp", d, 128);
  fclose (f);
  rdps_tls_small_keys_ok = 1;
  CHECK (rdps_make_tls_cert (ini, key, cert) == 0, "backpressure: a certificate");
  rdps_tls_small_keys_ok = 0;

  lfd = socket (AF_INET, SOCK_STREAM, 0);
  memset (&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
  bind (lfd, (struct sockaddr *) &sa, sizeof sa);
  listen (lfd, 1);
  getsockname (lfd, (struct sockaddr *) &sa, &sl);
  snprintf (port_s, sizeof port_s, "127.0.0.1:%d", ntohs (sa.sin_port));
  child = fork ();
  if (child == 0)
    {
      int o = open (out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      int nul = open ("/dev/null", O_RDWR);
      dup2 (nul, 0);
      dup2 (o, 1);
      dup2 (nul, 2);
      /* (SECLEVEL=0: the test key is 1024 bits) */
      execlp ("openssl", "openssl", "s_client", "-connect", port_s, "-quiet",
              "-no_tls1_3", "-cipher", "DEFAULT@SECLEVEL=0", (char *) NULL);
      _exit (127);
    }
  fd = accept (lfd, NULL, NULL);
  close (lfd);
  {
    int small = 4096;
    setsockopt (fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
  }
  fcntl (fd, F_SETFL, fcntl (fd, F_GETFL) | O_NONBLOCK);
  memset (&t, 0, sizeof t);
  t.sck = fd;
  tls = ssl_tls_create (&t, key, cert);
  CHECK (tls && ssl_tls_accept (tls, 0, "") == 0, "backpressure: the handshake with openssl");
  kill (child, SIGSTOP);

  for (i = 0; i < (int) sizeof data; i++)
    data[i] = (unsigned char) (i * 7 + (i >> 9));
  while (sent < sizeof data)
    {
      int r = ssl_tls_write (tls, (const char *) data + sent, (int) (sizeof data - sent));
      if (r > 0)
        sent += (size_t) r;
      else if (r < 0 && errno == EWOULDBLOCK)
        {
          would++;
          if (stopped && would >= 20)
            {
              kill (child, SIGCONT);    /* the link clears */
              stopped = 0;
            }
          g_sck_can_send (fd, 100);
        }
      else
        break;
    }
  if (stopped)
    kill (child, SIGCONT);
  CHECK (sent == sizeof data, "backpressure: all 2 MB written");
  CHECK (would >= 20, "backpressure: writes came back 'would block' and were retried");
  ssl_tls_disconnect (tls);
  ssl_tls_delete (tls);
  rdps_tls_forget ();
  shutdown (fd, SHUT_WR);
  for (i = 0; i < 100 && waitpid (child, &status, WNOHANG) == 0; i++)
    usleep (50000);
  if (i == 100)
    {
      kill (child, SIGKILL);
      waitpid (child, &status, 0);
    }
  close (fd);
  {
    size_t len = 0, k;
    static unsigned char got[sizeof data + 1];
    int same = 0;
    f = fopen (out, "rb");
    if (f)
      {
        len = fread (got, 1, sizeof got, f);
        fclose (f);
      }
    if (len == sizeof data)
      {
        same = 1;
        for (k = 0; k < len && same; k++)
          same = got[k] == data[k];
      }
    CHECK (same, "backpressure: openssl got every byte, in order, once");
    if (!same)
      printf ("  (received %lu bytes)\n", (unsigned long) len);
  }
  remove (out);
  remove (key);
  remove (cert);
  remove (ini);
  rmdir (dir);
}

/* --- input pacing ---------------------------------------------------------- */

static char trace[2048];
static void t_move (void *c, int x, int y) { (void) c; sprintf (trace + strlen (trace), "m%d,%d ", x, y); }
static void t_key (void *c, int ll, int d) { (void) c; sprintf (trace + strlen (trace), "k%02X%c ", ll, d ? 'd' : 'u'); }
static void t_wheel (void *c, int dz, int dw) { (void) c; sprintf (trace + strlen (trace), "w%d,%d ", dz, dw); }
static int text_full;
static int t_text (void *c, int ch)
{
  (void) c;
  if (text_full)
    return -1;
  sprintf (trace + strlen (trace), "c%02X ", ch);
  return 0;
}

static int click_ok = 1;
static int t_click (void *c, int ll)
{
  (void) c;
  if (!click_ok)
    return -1;
  sprintf (trace + strlen (trace), "C%02X ", ll);
  return 0;
}
static unsigned fake_clock, clock_reads;
static unsigned t_now (void *c) { (void) c; clock_reads++; return ++fake_clock; }

/* Characters sent as themselves (0.2-test23): RISC OS Latin-1, in order
   with the keys. */
static void test_unicode_chars (void)
{
  inq q;
  inq_sink ks = { t_move, t_key, t_wheel, NULL, NULL, t_text, NULL };
  CHECK (kbd_unicode_char ('a') == 'a' && kbd_unicode_char ('~') == '~', "ASCII as it is");
  CHECK (kbd_unicode_char (0xE9) == 0xE9 && kbd_unicode_char (0xA3) == 0xA3, "Latin-1 as it is");
  CHECK (kbd_unicode_char (0x20AC) == 0x80, "the euro at &80, as RISC OS has it");
  CHECK (kbd_unicode_char (0x201C) == 0x94 && kbd_unicode_char (0x2014) == 0x98
         && kbd_unicode_char (0x0153) == 0x9B, "quotes, dashes, oe");
  CHECK (kbd_unicode_char (13) == 13 && kbd_unicode_char (8) == 8 && kbd_unicode_char (127) == 127,
         "Return, Backspace, Delete");
  CHECK (kbd_unicode_char (0x85) == -1 && kbd_unicode_char (1) == -1,
         "C1 and other controls: none");
  CHECK (kbd_unicode_char (0x4E2D) == -1 && kbd_unicode_char (0x1F600) == -1,
         "characters RISC OS hasn't got: none");
  inq_init (&q);
  trace[0] = 0;
  inq_key (&q, 0x4C, 1);
  inq_char (&q, 'x');
  inq_key (&q, 0x4C, 0);
  inq_char (&q, 300);
  inq_char (&q, 'y');
  inq_run (&q, 1000, &ks);
  inq_run (&q, 1000 + INQ_BETWEEN_CS, &ks);
  CHECK (strcmp (trace, "k4Cd c78 k4Cu c79 ") == 0, "characters in their turn with the keys; one out of range dropped");
  /* the keyboard buffer full: the character waits, and what's after it */
  trace[0] = 0;
  text_full = 1;
  inq_char (&q, 'a');
  inq_key (&q, 0x3C, 1);
  CHECK (inq_run (&q, 2000, &ks) == 2 && trace[0] == 0, "buffer full: the character and the key after it wait");
  text_full = 0;
  inq_run (&q, 2000, &ks);
  CHECK (strcmp (trace, "c61 k3Cd ") == 0, "then go, in order");
  {
    unsigned char b[4];
    CHECK (kbd_unicode_bytes (0xFC, KBD_ALPHABET_LATIN1, b) == 1 && b[0] == 0xFC, "Latin1: u-umlaut &FC");
    CHECK (kbd_unicode_bytes (0x20AC, KBD_ALPHABET_LATIN1, b) == 1 && b[0] == 0x80, "Latin1: the euro &80");
    CHECK (kbd_unicode_bytes (0xFC, KBD_ALPHABET_UTF8, b) == 2 && b[0] == 0xC3 && b[1] == 0xBC,
           "UTF8: u-umlaut C3 BC");
    CHECK (kbd_unicode_bytes (0x20AC, KBD_ALPHABET_UTF8, b) == 3 && b[0] == 0xE2 && b[1] == 0x82
           && b[2] == 0xAC, "UTF8: the euro E2 82 AC");
    CHECK (kbd_unicode_bytes (0x1F600, KBD_ALPHABET_UTF8, b) == 4 && b[0] == 0xF0, "UTF8: 4 bytes past the BMP");
    CHECK (kbd_unicode_bytes (0xD83D, KBD_ALPHABET_UTF8, b) == 0, "UTF8: half a surrogate pair: none");
    CHECK (kbd_unicode_bytes ('a', KBD_ALPHABET_UTF8, b) == 1 && b[0] == 'a', "UTF8: ASCII");
    CHECK (kbd_unicode_bytes (0x20AC, KBD_ALPHABET_LATIN9, b) == 1 && b[0] == 0xA4, "Latin9: the euro &A4");
    CHECK (kbd_unicode_bytes (0xA4, KBD_ALPHABET_LATIN9, b) == 0, "Latin9: the currency sign isn't in it");
    CHECK (kbd_unicode_bytes (0xE9, KBD_ALPHABET_LATIN9, b) == 1 && b[0] == 0xE9, "Latin9: e-acute as Latin-1");
    CHECK (kbd_unicode_bytes (0xE9, 102, b) == 0 && kbd_unicode_bytes ('q', 102, b) == 1,
           "another alphabet (Latin2): ASCII only");
  }
}

/* RDPServer$Clicks 0 and RDPServer$ClickTime (0.2-test25) */
static void test_click_modes (void)
{
  inq q;
  inq_sink kk = { t_move, t_key, t_wheel, t_now, t_click, NULL, NULL };
  unsigned t = 5000;
  /* plain: the button goes down without waiting for its up, and up later */
  inq_init (&q);
  q.plain = 1;
  trace[0] = 0;
  inq_key (&q, 0x70, 1);
  inq_run (&q, t, &kk);
  CHECK (strcmp (trace, "k70d ") == 0, "plain: down at once, no waiting for the up, no ticker click");
  inq_key (&q, 0x70, 0);
  inq_run (&q, t + 1, &kk);
  CHECK (strcmp (trace, "k70d ") == 0, "plain: the up paced like a key");
  inq_run (&q, t + INQ_BETWEEN_CS, &kk);
  CHECK (strcmp (trace, "k70d k70u ") == 0, "plain: then up");
  /* a longer click: the next change waits for it */
  inq_init (&q);
  CHECK (q.click_cs == INQ_CLICK_CS, "the click time starts at INQ_CLICK_CS");
  q.click_cs = 20;
  trace[0] = 0;
  inq_key (&q, 0x70, 1);
  inq_key (&q, 0x70, 0);
  inq_key (&q, 0x3C, 1);
  inq_run (&q, t, &kk);
  CHECK (strncmp (trace, "C70 ", 4) == 0 && !strstr (trace, "k3C"), "a click, the key after it waits");
  inq_run (&q, t + 15, &kk);
  CHECK (!strstr (trace, "k3C"), "still waiting at 15 cs (a 20 cs click)");
  inq_run (&q, t + 21 + INQ_BETWEEN_CS, &kk);
  CHECK (strstr (trace, "k3Cd") != NULL, "and goes once the click's up is past (and the usual gap)");
}

static void test_input (void)
{
  inq q;
  inq_sink k = { t_move, t_key, t_wheel, NULL, NULL };
  inq_sink kc = { t_move, t_key, t_wheel, t_now, NULL };
  unsigned t = 1000;
  int i, left_down;

  /* A click as clients send it: move, down, up, all at once. */
  inq_init (&q);
  trace[0] = 0;
  inq_move (&q, 10, 20);
  inq_key (&q, 0x70, 1);
  inq_key (&q, 0x70, 0);
  CHECK (inq_run (&q, t, &k) == 2, "the move goes, the button waits");
  CHECK (strcmp (trace, "m10,20 ") == 0, "only the move so far");
  inq_run (&q, t + INQ_AFTER_MOVE_CS - 1, &k);
  CHECK (strcmp (trace, "m10,20 ") == 0, "not before the pointer has caught up");
  inq_run (&q, t + INQ_AFTER_MOVE_CS, &k);
  CHECK (strcmp (trace, "m10,20 k70d ") == 0, "then the button goes down");
  inq_run (&q, t + INQ_AFTER_MOVE_CS + INQ_BETWEEN_CS - 1, &k);
  CHECK (strcmp (trace, "m10,20 k70d ") == 0, "and stays down a moment");
  CHECK (inq_run (&q, t + INQ_AFTER_MOVE_CS + INQ_BETWEEN_CS, &k) == 0, "then comes up");
  CHECK (strcmp (trace, "m10,20 k70d k70u ") == 0, "down then up, in order");

  /* A double-click: the button must never be left down when inq_run
     returns (the Filer would take it as double-click-and-hold, that is
     Shift). With a clock, the down-up gap is waited out inside. */
  inq_init (&q);
  trace[0] = 0;
  inq_move (&q, 10, 20);
  inq_key (&q, 0x70, 1); inq_key (&q, 0x70, 0);
  inq_key (&q, 0x70, 1); inq_key (&q, 0x70, 0);
  left_down = 0;
  for (i = 0; i < 20 && q.count > 0; i++)
    {
      fake_clock = t + 2 * (unsigned) i;
      inq_run (&q, fake_clock, &kc);
      if (q.applied[0x70])
        left_down = 1;
    }
  CHECK (strcmp (trace, "m10,20 k70d k70u k70d k70u ") == 0, "double-click: both clicks, in order");
  CHECK (!left_down, "the button is never left down between calls");
  CHECK (clock_reads > 0, "the gap is waited out with the clock");

  /* A down whose up hasn't arrived yet waits for it. */
  inq_init (&q);
  trace[0] = 0;
  inq_move (&q, 10, 20);
  inq_run (&q, t, &kc);
  inq_key (&q, 0x70, 1);
  inq_run (&q, t + 10, &kc);
  inq_run (&q, t + 20, &kc);
  CHECK (strcmp (trace, "m10,20 ") == 0, "a down without its up waits");
  inq_move (&q, 12, 21);              /* a wobble, not a drag */
  inq_run (&q, t + 30, &kc);
  CHECK (strcmp (trace, "m10,20 ") == 0, "a 2-pixel wobble doesn't end the wait");
  inq_key (&q, 0x70, 0);
  fake_clock = t + 31;
  inq_run (&q, t + 31, &kc);
  CHECK (strcmp (trace, "m10,20 k70d m12,21 k70u ") == 0, "its up arrives: both go in one call");
  CHECK (!q.applied[0x70], "and nothing is left down");

  /* Held: after INQ_HOLD_CS the down goes on its own. */
  inq_init (&q);
  trace[0] = 0;
  inq_key (&q, 0x70, 1);
  inq_run (&q, t, &kc);
  inq_run (&q, t + INQ_HOLD_CS - 1, &kc);
  CHECK (trace[0] == 0, "held: waits up to INQ_HOLD_CS");
  inq_run (&q, t + INQ_HOLD_CS, &kc);
  CHECK (strcmp (trace, "k70d ") == 0 && q.applied[0x70], "then goes down alone");

  /* A drag: a down then a move away goes down at once. */
  inq_init (&q);
  trace[0] = 0;
  inq_move (&q, 100, 100);
  inq_run (&q, t, &kc);
  inq_key (&q, 0x70, 1);
  inq_move (&q, 120, 100);
  inq_run (&q, t + 10, &kc);
  CHECK (strcmp (trace, "m100,100 k70d m120,100 ") == 0, "a drag starts straight away");

  /* With a click sink (RISC OS: up by ticker), a click whose up is queued
     goes as one click, and the next change waits for that up. */
  {
    inq_sink kk = { t_move, t_key, t_wheel, t_now, t_click, NULL };
    unsigned first = 0, second = 0;
    inq_init (&q);
    trace[0] = 0;
    inq_move (&q, 10, 20);
    inq_key (&q, 0x70, 1); inq_move (&q, 10, 20); inq_key (&q, 0x70, 0);
    inq_key (&q, 0x70, 1); inq_key (&q, 0x70, 0);
    for (i = 0; i < 40 && q.count > 0; i++)
      {
        size_t before = strlen (trace);
        fake_clock = t + (unsigned) i;
        inq_run (&q, fake_clock, &kk);
        if (strlen (trace) != before && strstr (trace + before, "C70"))
          {
            if (!first) first = fake_clock; else second = fake_clock;
          }
        CHECK (!q.applied[0x70], "a click never leaves the button marked down");
      }
    CHECK (strcmp (trace, "m10,20 C70 m10,20 C70 ") == 0, "double-click: two whole clicks");
    CHECK (second >= first + INQ_CLICK_CS + 1 + INQ_BETWEEN_CS,
           "the second waits until the first's up has happened");
    CHECK (q.count == 0, "nothing left over (the ups were taken by the clicks)");

    /* A drag that arrives all at once (down, a long move, up) isn't a click. */
    inq_init (&q);
    trace[0] = 0;
    inq_move (&q, 10, 20);
    inq_run (&q, t, &kk);
    inq_key (&q, 0x70, 1); inq_move (&q, 60, 20); inq_key (&q, 0x70, 0);
    for (i = 0; i < 20 && q.count > 0; i++)
      {
        fake_clock = t + 10 + (unsigned) i;
        inq_run (&q, fake_clock, &kk);
      }
    CHECK (strcmp (trace, "m10,20 k70d m60,20 k70u ") == 0, "a batched drag stays a drag");

    /* Nor is a down with another key between it and its up. */
    inq_init (&q);
    trace[0] = 0;
    inq_key (&q, 0x70, 1); inq_key (&q, 0x4C, 1); inq_key (&q, 0x70, 0);
    for (i = 0; i < 20 && q.count > 0; i++)
      {
        fake_clock = t + 50 + (unsigned) i;
        inq_run (&q, fake_clock, &kk);
      }
    CHECK (strcmp (trace, "k70d k4Cd k70u ") == 0, "keys between keep their order");

    /* After a click, the wheel waits for its up. */
    inq_init (&q);
    trace[0] = 0;
    inq_key (&q, 0x70, 1); inq_key (&q, 0x70, 0); inq_wheel (&q, 120, 0);
    fake_clock = t + 80;
    inq_run (&q, t + 80, &kk);
    CHECK (strcmp (trace, "C70 ") == 0 && q.clicked, "the click, flagged for the caller");
    inq_run (&q, t + 80 + INQ_CLICK_CS, &kk);
    CHECK (strcmp (trace, "C70 ") == 0, "the wheel waits while the up is to come");
    inq_run (&q, t + 80 + INQ_CLICK_CS + 1, &kk);
    CHECK (strcmp (trace, "C70 w-1,0 ") == 0, "then turns");

    /* No ticker (click fails): down and up go separately, as before. */
    click_ok = 0;
    inq_init (&q);
    trace[0] = 0;
    inq_key (&q, 0x70, 1); inq_key (&q, 0x70, 0);
    for (i = 0; i < 10 && q.count > 0; i++)
      {
        fake_clock = t + 100 + (unsigned) i;
        inq_run (&q, fake_clock, &kk);
      }
    CHECK (strcmp (trace, "k70d k70u ") == 0, "without a ticker: down, then up");
    click_ok = 1;

    /* Held: down alone, later up alone. */
    inq_init (&q);
    trace[0] = 0;
    inq_key (&q, 0x72, 1);
    inq_run (&q, t, &kk);
    inq_run (&q, t + INQ_HOLD_CS, &kk);
    inq_key (&q, 0x72, 0);
    inq_run (&q, t + INQ_HOLD_CS + 10, &kk);
    CHECK (strcmp (trace, "k72d k72u ") == 0, "a held button isn't a click");
  }

  /* Moves in a row are merged; held keys aren't pressed twice. */
  inq_init (&q);
  trace[0] = 0;
  inq_move (&q, 1, 1);
  inq_move (&q, 2, 2);
  inq_move (&q, 3, 3);
  inq_key (&q, 0x3C, 1);
  inq_key (&q, 0x3C, 1);
  inq_key (&q, 0x3C, 1);
  inq_key (&q, 0x4C, 0);         /* up for a key that isn't down */
  inq_run (&q, 0, &k);
  inq_run (&q, 100, &k);
  CHECK (strcmp (trace, "m3,3 k3Cd ") == 0, "one move, one key-down");

  /* Releasing everything: what was pressed comes up at once. */
  trace[0] = 0;
  inq_key (&q, 0x4C, 1);
  inq_run (&q, 200, &k);
  inq_key (&q, 0x70, 1);           /* queued, not yet given to RISC OS */
  inq_key (&q, 0x70, 0);
  inq_release_all (&q, &k);
  CHECK (strstr (trace, "k3Cu") && strstr (trace, "k4Cu") && !strstr (trace, "k70"),
         "pressed keys released; the queued button never pressed");
  CHECK (q.count == 0, "nothing left waiting");

  /* The wheel: 120 a notch, small steps add up, sign as RISC OS wants. */
  inq_init (&q);
  trace[0] = 0;
  inq_wheel (&q, 120, 0);
  inq_wheel (&q, -240, 0);
  inq_wheel (&q, 60, 0);
  inq_wheel (&q, 60, 0);
  inq_wheel (&q, 120, 1);
  inq_run (&q, 0, &k);
  CHECK (strcmp (trace, "w-1,0 w2,0 w-1,0 w0,1 ") == 0, "wheel notches");
}

static void test_auth (void)
{
  const char *f = "build/tests/logins-test";
  auth_db db;
  FILE *o;

  remove (f);
  CHECK (auth_load (f, &db) == 0 && db.count == 0, "no file: no logins");
  CHECK (auth_set (f, "andrew", "pass word") == 0, "set a login");
  CHECK (auth_load (f, &db) == 0 && db.count == 1, "read it back");
  CHECK (auth_check (&db, "andrew", "pass word"), "right password");
  CHECK (auth_check (&db, "Andrew", "pass word"), "name in any case");
  CHECK (!auth_check (&db, "andrew", "pass Word"), "password is case-sensitive");
  CHECK (!auth_check (&db, "andrew", ""), "empty password");
  CHECK (!auth_check (&db, "bob", "pass word"), "unknown name");
  CHECK (auth_set (f, "two words", "x") != 0, "no spaces in a name");
  o = fopen (f, "r");
  {
    char buf[512];
    size_t n = fread (buf, 1, sizeof buf - 1, o);
    buf[n] = 0;
    CHECK (strstr (buf, "pass word") == NULL, "the password isn't stored");
  }
  fclose (o);
  o = fopen (f, "w");
  fputs ("andrew zz zz\n", o);
  fclose (o);
  CHECK (auth_load (f, &db) == -1 && db.count == 0, "a garbled file is an error, not open access");
  o = fopen (f, "w");   /* a salt one digit too long */
  fputs ("andrew 000000000000000000000000000000001 "
         "0000000000000000000000000000000000000000000000000000000000000000\n", o);
  fclose (o);
  CHECK (auth_load (f, &db) == -1, "wrong-length hex is an error");
  remove (f);
}

int main (void)
{
  test_input ();
  test_auth ();
  test_pixels ();
  test_notice ();
  test_solid ();
  test_capture ();
  test_unicode_chars ();
  test_click_modes ();
  test_capture_sampled ();
  test_pointer ();
  test_kbdlayout ();
  test_login ();
  test_utf8 ();
  test_keymap ();
  test_crypto ();
  test_keygen ();
  test_tls_cert ();
  test_tls_backpressure ();
  printf ("RDPServer units: %d checks, %d failed\n", checks, fails);
  return fails != 0;
}
