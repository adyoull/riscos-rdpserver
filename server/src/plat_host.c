/* RDPServer: a pretend RISC OS screen on Linux, so the server can be tested
   with real RDP clients (rdesktop, FreeRDP) in a container.

   The screen is 32 bpp TBGR (as a RISC OS desktop is), 640x480 unless
   RDPS_HOST_SIZE says otherwise ("WxH"); a pretend mode change
   (RDPS_HOST_MODE_FILE, below) can give it another size and any RISC OS
   pixel format, palette ones included. It shows four coloured quarters
   and a white square that moves one tile to the right every second, so a
   client's picture can be checked pixel by pixel. Input goes to the file
   named by RDPS_HOST_INPUT_LOG ("mouse X Y" and "key LL down|up" lines).

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "platform.h"

static unsigned char *screen;
static int sw = 640, sh = 480;
static int spad;                 /* pixels past the end of each line */
static int sprite_reads;         /* this many reads see output going to a sprite */
static int square_at = -1;
/* the mode's pixel format: Log2BPP, NColour, ModeFlags */
static int hl2bpp = 5, hncolour = -1;
static unsigned hflags = 0x40;
/* palette modes: black, red, green, blue, grey, white, then black */
static uint32_t hpal[256] = { 0x000000, 0xff0000, 0x00ff00, 0x0000ff, 0x808080, 0xffffff };

static int
hbits (void)
{
  return 1 << hl2bpp;
}

static size_t
hstride (void)
{
  return ((size_t) (sw + spad) * (size_t) hbits () + 7) / 8;
}

/* One pixel in the mode's format (palette modes: the entry with that
   colour, or 0). */
static void
put (int x, int y, unsigned r, unsigned g, unsigned b)
{
  unsigned char *row = screen + (size_t) y * hstride ();
  int rgb = (hflags & 0x4000) != 0, bits = hbits ();
  if (bits == 32)
    {
      unsigned char *p = row + (size_t) x * 4;
      p[0] = (unsigned char) (rgb ? b : r); p[1] = (unsigned char) g;
      p[2] = (unsigned char) (rgb ? r : b); p[3] = 0;
    }
  else if (bits == 16)
    {
      unsigned v, lo = rgb ? b : r, hi = rgb ? r : b;
      unsigned char *p = row + (size_t) x * 2;
      if (hncolour == 4095)
        v = (lo >> 4) | ((g >> 4) << 4) | ((hi >> 4) << 8);
      else if (hflags & 0x80)
        v = (lo >> 3) | ((g >> 2) << 5) | ((hi >> 3) << 11);
      else
        v = (lo >> 3) | ((g >> 3) << 5) | ((hi >> 3) << 10);
      p[0] = (unsigned char) v; p[1] = (unsigned char) (v >> 8);
    }
  else
    {
      unsigned want = (r << 16) | (g << 8) | b, i, n = 1u << bits, at;
      unsigned mask = n - 1;
      for (i = 0; i < n && hpal[i] != want; i++)
        ;
      if (i == n)
        i = 0;
      at = (unsigned) x * (unsigned) bits;
      row[at >> 3] = (unsigned char) ((row[at >> 3] & ~(mask << (at & 7))) | (i << (at & 7)));
    }
}

/* A system variable: RISC OS's "RDPServer$SendBuffer" is the environment's
   "RDPServer_SendBuffer". */
int
plat_var_int (const char *name, int def)
{
  char n[64];
  const char *v;
  size_t i;
  for (i = 0; name[i] && i < sizeof n - 1; i++)
    n[i] = name[i] == '$' ? '_' : name[i];
  n[i] = 0;
  v = getenv (n);
  return v && *v ? atoi (v) : def;
}

/* The pretend RISC OS keyboard: RDPS_HOST_KEYBOARD, or UK. */
int
plat_keyboard (void)
{
  const char *k = getenv ("RDPS_HOST_KEYBOARD");
  return k ? atoi (k) : 1;
}

int
plat_palette (uint32_t rgb[256])
{
  int n = hbits () <= 8 ? 1 << hbits () : 0;
  memcpy (rgb, hpal, (size_t) n * sizeof *rgb);
  return n;
}

static void
draw (int at)
{
  int x, y;
  if (getenv ("RDPS_HOST_NOISE"))
    {
      /* RDPS_HOST_NOISE: a new screenful of noise every second (nothing
         compresses it), for tests of a client that stops reading */
      static unsigned seed = 12345;
      size_t i, n = hstride () * (size_t) sh;
      for (i = 0; i < n; i++)
        {
          seed = seed * 1103515245u + 12345u;
          screen[i] = (unsigned char) (seed >> 16);
        }
      return;
    }
  for (y = 0; y < sh; y++)
    for (x = 0; x < sw; x++)
      {
        int right = x >= sw / 2, low = y >= sh / 2;
        if (!right && !low) put (x, y, 0xff, 0, 0);          /* red */
        else if (right && !low) put (x, y, 0, 0xff, 0);      /* green */
        else if (!right && low) put (x, y, 0, 0, 0xff);      /* blue */
        else put (x, y, 0x80, 0x80, 0x80);                   /* grey */
      }
  for (y = 64; y < 128 && y < sh; y++)
    for (x = at * 64; x < at * 64 + 64 && x < sw; x++)
      put (x, y, 0xff, 0xff, 0xff);
}

unsigned
plat_time_cs (void)
{
  struct timespec t;
  clock_gettime (CLOCK_MONOTONIC, &t);
  return (unsigned) (t.tv_sec * 100 + t.tv_nsec / 10000000);
}

const char *
plat_screen (rdps_screen *s)
{
  int at;
  if (!screen)
    {
      const char *size = getenv ("RDPS_HOST_SIZE");
      if (size)
        sscanf (size, "%dx%d", &sw, &sh);
      screen = malloc (hstride () * sh);
      if (!screen)
        return "no memory for the fake screen";
    }
  {
    /* A pretend mode change, for tests: RDPS_HOST_MODE_FILE names a file
       holding "WxH"; when it appears the screen takes that size (at a new
       address, as a real mode change moves it) and the file is removed. */
    const char *mf = getenv ("RDPS_HOST_MODE_FILE");
    FILE *f = mf ? fopen (mf, "r") : NULL;
    if (f)
      {
        int w = 0, h = 0;
        unsigned char *n;
        char word[8] = "";
        long at0 = ftell (f);
        /* "bank": the same mode, another screen bank shown (OS_Byte 113) */
        /* "sprite N": the next N reads find output switched to a sprite
           (another program's turn, OS_SpriteOp 60): its mode */
        if (fscanf (f, "%7s", word) == 1 && strcmp (word, "sprite") == 0)
          {
            if (fscanf (f, "%d", &sprite_reads) != 1)
              sprite_reads = 100;
          }
        /* "pal": a program changes the palette (red becomes yellow) */
        else if (strcmp (word, "pal") == 0)
          hpal[1] = 0xffff00;
        else if (strcmp (word, "bank") == 0)
          {
            n = malloc (hstride () * sh);
            if (n)
              {
                memcpy (n, screen, hstride () * sh);
                free (screen);
                screen = n;
              }
          }
        /* "WxH", then any of: "pad" (each line 32 pixels longer than it
           shows: another LineLength, as a mode at the same size can
           have); a depth, "1", "2", "4", "8" (palette), "4k", "32k",
           "64k" or "16m"; "rgb" (RGB order, ModeFlags bit 14) */
        else if (fseek (f, at0, SEEK_SET) == 0 && fscanf (f, "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64 && w <= 4096 && h <= 4096)
          {
            int pad = 0, l2 = 5, nc = -1;
            unsigned fl = 0x40;
            while (fscanf (f, "%7s", word) == 1)
              {
                if (strcmp (word, "pad") == 0) pad = 32;
                else if (strcmp (word, "1") == 0) { l2 = 0; nc = 1; fl = 0; }
                else if (strcmp (word, "2") == 0) { l2 = 1; nc = 3; fl = 0; }
                else if (strcmp (word, "4") == 0) { l2 = 2; nc = 15; fl = 0; }
                else if (strcmp (word, "8") == 0) { l2 = 3; nc = 255; fl = 0x80; }
                else if (strcmp (word, "4k") == 0) { l2 = 4; nc = 4095; fl = 0x80; }
                else if (strcmp (word, "32k") == 0) { l2 = 4; nc = 65535; fl = 0; }
                else if (strcmp (word, "64k") == 0) { l2 = 4; nc = 65535; fl = 0x80; }
                else if (strcmp (word, "16m") == 0) { l2 = 5; nc = -1; fl = 0x40; }
                else if (strcmp (word, "rgb") == 0) fl |= 0x4000;
              }
            {
              size_t stride = ((size_t) (w + pad) * (size_t) (1 << l2) + 7) / 8;
              if ((n = malloc (stride * h)) != NULL)
                {
                  free (screen);
                  screen = n;
                  memset (screen, 0x55, stride * h);
                  sw = w;
                  sh = h;
                  spad = pad;
                  hl2bpp = l2;
                  hncolour = nc;
                  hflags = fl;
                  square_at = -1;
                }
            }
          }
        fclose (f);
        remove (mf);
      }
  }
  at = getenv ("RDPS_HOST_STILL") ? 0 : (int) ((plat_time_cs () / 100) % (unsigned) (sw / 64));
  if (at != square_at)
    {
      draw (at);
      square_at = at;
    }
  s->width = sw;
  s->height = sh;
  s->log2bpp = hl2bpp;
  s->ncolour = hncolour;
  s->modeflags = hflags;
  s->xeig = s->yeig = 1;
  s->stride = hstride ();
  s->base = screen;
  s->total = hstride () * sh;
  s->redirected = 0;
  if (sprite_reads > 0)
    {
      sprite_reads--;
      s->width = 320;
      s->height = 200;
      s->log2bpp = 3;
      s->ncolour = 255;
      s->modeflags = 0;
      s->stride = 320;
      s->redirected = 1;            /* base stays the screen (DisplayStart) */
    }
  return NULL;
}

static void
logline (const char *fmt, int a, int b)
{
  const char *f = getenv ("RDPS_HOST_INPUT_LOG");
  FILE *o;
  if (!f || !(o = fopen (f, "a")))
    return;
  fprintf (o, fmt, a, b);
  fclose (o);
}

void
plat_mouse_to (const rdps_screen *s, int x, int y)
{
  (void) s;
  logline ("mouse %d %d\n", x, y);
}

int
plat_char (int c)
{
  logline ("char %02X\n", c, 0);
  return 0;
}

/* RDPS_HOST_ALPHABET: the alphabet number to pretend (101, Latin1). */
int
plat_alphabet (void)
{
  const char *a = getenv ("RDPS_HOST_ALPHABET");
  return a && *a ? atoi (a) : 101;
}

/* RDPS_HOST_STUCK_SHIFT pretends RISC OS thinks Shift is held (as the
   Pi did with test2) until a Shift key is released. */
static int shift_down = -1;

void
plat_key (int low_level_key, int down)
{
  logline (down ? "key %02X down\n" : "key %02X up\n", low_level_key, 0);
  if (low_level_key == 0x4C || low_level_key == 0x58)
    shift_down = down;
}

/* The pretend RISC OS has no Filer: a click is a down and an up. */
int
plat_click (int ll, unsigned up_after_cs)
{
  (void) up_after_cs;
  plat_key (ll, 1);
  logline ("click-up-later %02X\n", ll, 0);
  plat_key (ll, 0);
  return 0;
}

void
plat_click_finish (void)
{
}

int
plat_modifiers (void)
{
  if (shift_down < 0)
    shift_down = getenv ("RDPS_HOST_STUCK_SHIFT") != NULL;
  return shift_down ? PLAT_SHIFT : 0;
}

void
plat_wheel (int dz, int dw)
{
  logline ("wheel %d %d\n", dz, dw);
}

void
plat_set_clients (int n)
{
  logline ("clients %d\n", n, 0);
}

const char *
plat_resources_dir (void)
{
  const char *d = getenv ("RDPS_RESOURCES");
  return d ? d : ".";
}

const char *
plat_choices_dir (void)
{
  const char *d = getenv ("RDPS_CHOICES");
  return d ? d : ".";
}

/* --- sound (tests): a 440 Hz tone at 48 kHz, made as it would be played:
   as much as real time has passed since the start. RDPS_HOST_SILENT set:
   silence instead (the server skips silent blocks). Mute is logged. */
static int snd_on;
static unsigned snd_start_cs;
static unsigned long snd_made;          /* frames handed out */

int
plat_sound_rate (unsigned *rate_hz)
{
  *rate_hz = 48000;
  return 0;
}

int
plat_sound_start (unsigned *rate_hz)
{
  snd_on = 1;
  snd_start_cs = plat_time_cs ();
  snd_made = 0;
  *rate_hz = 48000;
  return 0;
}

int
plat_sound_read (void *buf, int bytes, int max_behind)
{
  unsigned long played;
  int frames = bytes / 4, i;
  short *p = buf;
  if (!snd_on || bytes <= 0 || (bytes & 3))
    return 0;
  played = (unsigned long) (plat_time_cs () - snd_start_cs) * 480;
  (void) max_behind;
  if (played - snd_made > 48000)        /* behind by over a second: catch up */
    snd_made = played - 24000;
  if (played - snd_made < (unsigned long) frames)
    return 0;
  for (i = 0; i < frames; i++)
    {
      /* a triangle wave near 440 Hz (no libm): period 109 frames */
      int ph = (int) ((snd_made + (unsigned long) i) % 109);
      int v = getenv ("RDPS_HOST_SILENT") ? 0
              : (ph < 55 ? ph * 400 - 11000 : (109 - ph) * 400 - 11000);
      p[2 * i] = (short) v;
      p[2 * i + 1] = (short) v;
    }
  snd_made += (unsigned long) frames;
  return bytes;
}

int
plat_sound_ok (void)
{
  return snd_on && !getenv ("RDPS_HOST_SOUND_TAKEN");
}

void
plat_sound_stats (plat_sound_info *i)
{
  i->calls = (unsigned) (snd_made / 1024);
  i->with_sound = getenv ("RDPS_HOST_SILENT") ? 0 : i->calls;
  i->rate = 48000 * 1024;
  i->flags = 1;
  i->written = (unsigned) snd_made * 4;
  i->before = 0;
  i->before_ss = 0;
  i->keep = 0;
}

void
plat_sound_mute (int on)
{
  logline ("sound mute %d\n", on, 0);
}

void
plat_sound_stop (void)
{
  snd_on = 0;
}

/* No NEON here (the module only). */
int plat_neon_start (void) { return 0; }
int plat_neon_enter (void) { return -1; }
void plat_neon_leave (void) { }
void plat_neon_reset (void) { }
void plat_neon_stop (void) { }
int
plat_neon_convert (int rgb, int client_bpp, void *dst, const void *src, unsigned pixels)
{
  (void) rgb; (void) client_bpp; (void) dst; (void) src; (void) pixels;
  return -1;
}
void
plat_neon_copy (void *dst, const void *src, unsigned bytes)
{
  (void) dst; (void) src; (void) bytes;
}

/* Memory: Linux's malloc. */
int plat_memory_reserve (unsigned bytes) { (void) bytes; return 0; }
int plat_memory_short (void) { return 0; }
void plat_memory_info (unsigned *used_kb, unsigned *max_kb, int *refused)
{ *used_kb = *max_kb = 0; *refused = 0; }
