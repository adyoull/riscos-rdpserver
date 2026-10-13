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
  CHECK (pix_format_of (3, 255, 0x80) == PIX_UNSUPPORTED, "8 bpp isn't shared");
  CHECK (pix_format_of (4, 4095, 0x80) == PIX_UNSUPPORTED, "16 bpp 4:4:4 isn't shared");
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
  pix_palette_332 (pal);
  CHECK (pal[0xe0] == 0xff0000 && pal[0x1c] == 0x00ff00 && pal[0x03] == 0x0000ff
         && pal[0xff] == 0xffffff && pal[0] == 0, "the 3:3:2 palette's corners");
}

static void test_capture (void)
{
  enum { W = 150, H = 70, BPP = 4 };   /* partial tiles both ways */
  size_t stride = W * BPP + 12;
  unsigned char *scr = calloc (H, stride);
  capture c;
  int i, x, y, w, h, n;

  CHECK (cap_init (&c, W, H, BPP) == 0, "cap_init");
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
  CHECK (cap_next_dirty (&c, &i, &x, &y, &w, &h) && x == 128 && y == 64 && w == 22 && h == 6,
         "the partial tile's rectangle");
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
  cap_free (&c);
  free (scr);
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

/* --- input pacing ---------------------------------------------------------- */

static char trace[2048];
static void t_move (void *c, int x, int y) { (void) c; sprintf (trace + strlen (trace), "m%d,%d ", x, y); }
static void t_key (void *c, int ll, int d) { (void) c; sprintf (trace + strlen (trace), "k%02X%c ", ll, d ? 'd' : 'u'); }
static void t_wheel (void *c, int dz, int dw) { (void) c; sprintf (trace + strlen (trace), "w%d,%d ", dz, dw); }

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
  test_capture ();
  test_keymap ();
  test_crypto ();
  test_keygen ();
  printf ("RDPServer units: %d checks, %d failed\n", checks, fails);
  return fails != 0;
}
