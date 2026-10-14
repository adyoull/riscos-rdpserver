/* RDPServer module: what the computer plays, for sound.c (platform.h's
   plat_sound_*). A SharedSound process handler (header.s,
   rdps_snd_handler) copies each mixed buffer into a ring in the dynamic
   area; the server thread reads it.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>

#include "../platform.h"

/* read and written by the handler in header.s (offsets there) */
struct snd_ring
{
  unsigned char *base;
  unsigned mask;
  volatile unsigned wr;         /* bytes written, in all */
  volatile unsigned rate;       /* as SharedSound last said */
  volatile unsigned flags;
  volatile int mute;
};
struct snd_ring rdps_snd;

extern void rdps_snd_handler (void);

#define RING (256 * 1024)       /* 1.3 s at 48 kHz */
#define XSharedSound_InstallHandler 0x6B440
#define XSharedSound_RemoveHandler 0x6B441
#define XSound_SampleRate 0x60146

static int handle = -1;
static unsigned rd;

int
plat_sound_rate (unsigned *rate_hz)
{
  int r2 = 0;
  /* the current 16-bit sample rate, 1/1024 Hz (PRM Sound_SampleRate 1) */
  if (_swix (XSound_SampleRate, _IN (0) | _OUT (2), 1, &r2) != NULL || r2 <= 0)
    return -1;
  *rate_hz = ((unsigned) r2 + 512) / 1024;
  return 0;
}

int
plat_sound_start (unsigned *rate_hz)
{
  int h = -1;
  unsigned char *ring;
  if (handle >= 0)
    plat_sound_stop ();
  if (plat_sound_rate (rate_hz) != 0)
    return -1;
  ring = malloc (RING);
  if (!ring)
    return -1;
  memset (ring, 0, RING);
  rdps_snd.base = ring;
  rdps_snd.mask = RING - 1;
  rdps_snd.wr = 0;
  rdps_snd.rate = 0;
  rdps_snd.flags = 0;
  rdps_snd.mute = 0;
  rd = 0;
  if (_swix (XSharedSound_InstallHandler, _INR (0, 4) | _OUT (0),
             (int) rdps_snd_handler, (int) &rdps_snd, 1, "RDPServer", 2, &h) != NULL
      || h < 0)
    {
      rdps_snd.base = NULL;
      free (ring);
      return -1;
    }
  handle = h;
  return 0;
}

int
plat_sound_read (void *buf, int bytes, int max_behind)
{
  unsigned wr = rdps_snd.wr, avail, at, first;
  unsigned char *b = buf;
  if (handle < 0 || bytes <= 0 || (bytes & 3))
    return 0;
  avail = wr - rd;
  /* fell behind (a stall, or overrun): keep just the newest two blocks
     (SharedSound's buffers are well under the ring's 32 KB slack) */
  if (avail > RING - 32768 || (max_behind > 0 && avail > (unsigned) max_behind))
    {
      rd = (wr - 2 * (unsigned) bytes) & ~3u;
      avail = wr - rd;
    }
  if (avail < (unsigned) bytes)
    return 0;
  at = rd & (RING - 1);
  first = RING - at < (unsigned) bytes ? RING - at : (unsigned) bytes;
  memcpy (b, rdps_snd.base + at, first);
  memcpy (b + first, rdps_snd.base, (size_t) bytes - first);
  rd += (unsigned) bytes;
  if (rdps_snd.flags & (1u << 29))  /* reverse stereo: put left first */
    {
      int i;
      for (i = 0; i < bytes; i += 4)
        {
          unsigned char t0 = b[i], t1 = b[i + 1];
          b[i] = b[i + 2];
          b[i + 1] = b[i + 3];
          b[i + 2] = t0;
          b[i + 3] = t1;
        }
    }
  return bytes;
}

void
plat_sound_mute (int on)
{
  rdps_snd.mute = on;
}

void
plat_sound_stop (void)
{
  if (handle >= 0)
    {
      _swix (XSharedSound_RemoveHandler, _IN (0), handle);
      handle = -1;
    }
  rdps_snd.mute = 0;
  if (rdps_snd.base)
    {
      unsigned char *r = rdps_snd.base;
      rdps_snd.base = NULL;     /* (the handler is gone first) */
      free (r);
    }
}
