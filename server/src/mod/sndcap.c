/* RDPServer module: what the computer plays, for sound.c (platform.h's
   plat_sound_*).

   SoundDMA's 16-bit linear handler makes the final mix: with any 16-bit
   sound playing that's SharedSound's, which runs its clients' handlers.
   The capture wraps it (header.s, rdps_lin_stub): a copy of the stub in
   the RMA becomes the linear handler, calls the one it replaced, and
   copies the result into a ring the server thread reads. A SharedSound
   process handler (rdps_keep_handler) is installed first only to keep
   SharedSound the linear handler meanwhile (it installs itself when it has
   a client), so it doesn't take over from the stub later.

   (0.2-test11 and 12 copied the sound in a SharedSound process handler,
   which only sees what SharedSound's callback handlers make: players that
   use immediate handlers never reached it, so Windows App heard nothing.)

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>

#include "../platform.h"

/* after the stub in the RMA; offsets in header.s (L_*) */
struct lin_ws
{
  void *old;                    /* the handler the stub replaced, or 0 */
  int old_ws;
  volatile int active;          /* 0: only pass through */
  unsigned char *base;          /* the ring */
  unsigned mask;
  volatile unsigned wr;         /* bytes written, in all */
  volatile int mute;
  volatile unsigned calls;
  volatile unsigned rate;       /* R4: 1/1024 Hz */
  volatile unsigned flags;      /* R3 */
  volatile unsigned with_sound; /* buffers that weren't all zeros */
};

extern char rdps_lin_stub[], rdps_keep_handler[], rdps_lin_stub_end[];

#define RING (256 * 1024)       /* 1.3 s at 48 kHz */
#define XOS_Module 0x2001E
#define XOS_SynchroniseCodeAreas 0x2006E
#define XSharedSound_InstallHandler 0x6B440
#define XSharedSound_RemoveHandler 0x6B441
#define XSound_LinearHandler 0x60145
#define XSound_SampleRate 0x60146

static int keep = -1;           /* the SharedSound handler */
static char *stub;              /* the RMA block: code, then lin_ws */
static struct lin_ws *ws;
static unsigned char *ring;
static unsigned rd;
static unsigned start_old;      /* the linear handler before us (for the log) */
static int start_old_ss;        /* that was SharedSound's */

static size_t
stub_code_size (void)
{
  return (size_t) (rdps_lin_stub_end - rdps_lin_stub);
}

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
  int h = -1, cur = 0, cur_ws = -1, old = 0, old_ws = -1;
  size_t code;
  if (stub)
    plat_sound_stop ();
  if (plat_sound_rate (rate_hz) != 0)
    return -1;
  ring = malloc (RING);
  if (!ring)
    return -1;
  memset (ring, 0, RING);
  rd = 0;

  code = (stub_code_size () + 3) & ~(size_t) 3;
  if (_swix (XOS_Module, _IN (0) | _IN (3) | _OUT (2), 6,
             (int) (code + sizeof (struct lin_ws)), &stub) != NULL || !stub)
    {
      stub = NULL;
      plat_sound_stop ();
      return -1;
    }
  memcpy (stub, rdps_lin_stub, stub_code_size ());
  ws = (struct lin_ws *) (void *) (stub + code);
  memset (ws, 0, sizeof *ws);
  ws->base = ring;
  ws->mask = RING - 1;
  _swix (XOS_SynchroniseCodeAreas, _INR (0, 2), 1, (int) stub,
         (int) (stub + code - 4));

  /* SharedSound first (if it's there): it's then the linear handler, and
     stays it while we're on top. Its handler is the RMA copy too. */
  if (_swix (XSharedSound_InstallHandler, _INR (0, 4) | _OUT (0),
             (int) (stub + (rdps_keep_handler - rdps_lin_stub)), 0, 1, "RDPServer",
             2, &h) == NULL && h > 0)
    keep = h;

  /* what's there now; then us, passing to it */
  if (_swix (XSound_LinearHandler, _IN (0) | _OUTR (1, 2), 0, &cur, &cur_ws) != NULL)
    {
      plat_sound_stop ();
      return -1;
    }
  ws->old = (void *) cur;
  ws->old_ws = cur_ws;
  start_old = (unsigned) cur;
  {
    /* SharedSound's? (for the log) */
    int base = 0;
    start_old_ss = cur && _swix (XOS_Module, _INR (0, 1) | _OUT (3), 18, "SharedSound",
                                 &base) == NULL && base
                   && (unsigned) cur >= (unsigned) base
                   && (unsigned) cur < (unsigned) base + 0x10000;
  }
  ws->active = 1;
  if (_swix (XSound_LinearHandler, _INR (0, 2) | _OUTR (1, 2), 1, (int) stub,
             (int) ws, &old, &old_ws) != NULL)
    {
      ws->active = 0;
      plat_sound_stop ();
      return -1;
    }
  if (old != cur || old_ws != cur_ws)
    {
      /* changed between the two calls (an interrupt): pass to that one */
      ws->old = (void *) old;
      ws->old_ws = old_ws;
    }
  return 0;
}

/* 1 while the stub is still SoundDMA's linear handler. */
int
plat_sound_ok (void)
{
  int cur = 0, cur_ws = 0;
  if (!stub)
    return 0;
  if (_swix (XSound_LinearHandler, _IN (0) | _OUTR (1, 2), 0, &cur, &cur_ws) != NULL)
    return 0;
  return cur == (int) stub && cur_ws == (int) ws;
}

int
plat_sound_read (void *buf, int bytes, int max_behind)
{
  unsigned wr, avail, at, first;
  unsigned char *b = buf;
  int i;
  if (!stub || !ring || bytes <= 0 || (bytes & 3))
    return 0;
  wr = ws->wr;
  avail = wr - rd;
  /* fell behind (a stall, or overrun): keep just the newest two blocks */
  if (avail > RING - 32768 || (max_behind > 0 && avail > (unsigned) max_behind))
    {
      rd = (wr - 2 * (unsigned) bytes) & ~3u;
      avail = wr - rd;
    }
  if (avail < (unsigned) bytes)
    return 0;
  at = rd & (RING - 1);
  first = RING - at < (unsigned) bytes ? RING - at : (unsigned) bytes;
  memcpy (b, ring + at, first);
  memcpy (b + first, ring, (size_t) bytes - first);
  rd += (unsigned) bytes;
  /* the linear handler's words: right in the low half, left in the high
     half; RDP's PCM: left first */
  for (i = 0; i < bytes; i += 4)
    {
      unsigned char t0 = b[i], t1 = b[i + 1];
      b[i] = b[i + 2];
      b[i + 1] = b[i + 3];
      b[i + 2] = t0;
      b[i + 3] = t1;
    }
  return bytes;
}

void
plat_sound_stats (plat_sound_info *i)
{
  memset (i, 0, sizeof *i);
  if (!ws)
    return;
  i->calls = ws->calls;
  i->with_sound = ws->with_sound;
  i->rate = ws->rate;
  i->flags = ws->flags;
  i->written = ws->wr;
  i->before = start_old;
  i->before_ss = start_old_ss;
  i->keep = keep > 0;
}

void
plat_sound_mute (int on)
{
  if (ws)
    ws->mute = on;
}

void
plat_sound_stop (void)
{
  int unhooked = 1;
  if (ws)
    {
      int cur = 0, cur_ws = 0, a, b;
      ws->mute = 0;
      ws->active = 0;           /* from now on it only passes through */
      ws->base = NULL;
      if (_swix (XSound_LinearHandler, _IN (0) | _OUTR (1, 2), 0, &cur, &cur_ws) == NULL
          && cur == (int) stub && cur_ws == (int) ws)
        /* still ours: put back what was there */
        _swix (XSound_LinearHandler, _INR (0, 2) | _OUTR (1, 2), 1, (int) ws->old,
               ws->old ? ws->old_ws : 0, &a, &b);
      else
        unhooked = 0;           /* another program took over after us and
                                   may call the stub: it stays */
    }
  if (keep > 0)
    {
      if (_swix (XSharedSound_RemoveHandler, _IN (0), keep) != NULL)
        unhooked = 0;           /* SharedSound may still call it: it stays */
      keep = -1;
    }
  /* The RMA copy goes only when nothing can call it any more; otherwise a
     few hundred bytes stay behind, passing through. Nothing ever calls
     the module's own code, so the module can always be killed. */
  if (stub && unhooked)
    _swix (XOS_Module, _IN (0) | _IN (2), 7, (int) stub);
  stub = NULL;
  ws = NULL;
  if (ring)
    {
      free (ring);
      ring = NULL;
    }
}
