/* RDPServer: a pretend RISC OS screen on Linux, so the server can be tested
   with real RDP clients (rdesktop, FreeRDP) in a container.

   The screen is 32 bpp TBGR (as a RISC OS desktop is), 640x480 unless
   RDPS_HOST_SIZE says otherwise ("WxH"). It shows four coloured quarters
   and a white square that moves one tile to the right every second, so a
   client's picture can be checked pixel by pixel. Input goes to the file
   named by RDPS_HOST_INPUT_LOG ("mouse X Y" and "key LL down|up" lines).

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "platform.h"

static unsigned char *screen;
static int sw = 640, sh = 480;
static int spad;                 /* pixels past the end of each line */
static int square_at = -1;

/* TBGR: red in the low byte. */
static void
put (int x, int y, unsigned r, unsigned g, unsigned b)
{
  unsigned char *p = screen + ((size_t) y * (sw + spad) + x) * 4;
  p[0] = (unsigned char) r; p[1] = (unsigned char) g; p[2] = (unsigned char) b; p[3] = 0;
}

static void
draw (int at)
{
  int x, y;
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
      screen = malloc ((size_t) sw * sh * 4);
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
        if (fscanf (f, "%7s", word) == 1 && strcmp (word, "bank") == 0)
          {
            n = malloc ((size_t) (sw + spad) * sh * 4);
            if (n)
              {
                memcpy (n, screen, (size_t) (sw + spad) * sh * 4);
                free (screen);
                screen = n;
              }
          }
        /* "WxH", or "WxH pad": each line 32 pixels longer than it shows
           (another LineLength, as a mode at the same size can have) */
        else if (fseek (f, at0, SEEK_SET) == 0 && fscanf (f, "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64 && w <= 4096 && h <= 4096)
          {
            int pad = fscanf (f, "%7s", word) == 1 && strcmp (word, "pad") == 0 ? 32 : 0;
            if ((n = malloc ((size_t) (w + pad) * h * 4)) != NULL)
              {
                free (screen);
                screen = n;
                memset (screen, 0x55, (size_t) (w + pad) * h * 4);
                sw = w;
                sh = h;
                spad = pad;
                square_at = -1;
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
  s->log2bpp = 5;
  s->ncolour = -1;
  s->modeflags = 0x40;
  s->xeig = s->yeig = 1;
  s->stride = (size_t) (sw + spad) * 4;
  s->base = screen;
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
