/* RDPServer: the module's own VFP context, for NEON (platform.h,
   0.2-test28).

   VFPSupport (PRM p700-709) keeps one context active at a time, the
   VFP/NEON registers of whichever program is using them. The module makes
   its own once (user mode access: the server thread runs in USR mode; 32
   doubleword registers; VFPSupport keeps the memory) and switches to it
   only around NEON work: there lazily (nothing is saved unless NEON is
   actually used), and back at once, so that the program it interrupted
   has its own registers loaded again and the module's context is never
   the loaded one when it's destroyed. (0.2-test28 switched back lazily
   too: killing the module then destroyed its context while it was still
   loaded, and VFPSupport put the null context in place of the program's,
   whose next VFP instruction faulted: !RDPServer's Quit.) NEON is used only if VFPSupport_Features 0 says the
   Advanced SIMD integer and load/store instructions are there (MVFR1
   bits 8-15).

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stddef.h>
#include <stdint.h>
#include <kernel.h>
#include <swis.h>
#include "platform.h"

#define XVFPSupport_CreateContext  0x78EC1
#define XVFPSupport_DestroyContext 0x78EC2
#define XVFPSupport_ChangeContext  0x78EC3
#define XVFPSupport_Features       0x78EC8

#define CTX_USER_MODE 1u          /* VFPSupport_CreateContext R0 bit 0 */
#define CHANGE_LAZY   1u          /* VFPSupport_ChangeContext R1 bit 0 */
#define CHANGE_NOW    0u

void rdps_neon_tbgr_to_32 (uint32_t *dst, const void *src, size_t pixels);
void rdps_neon_trgb_to_32 (uint32_t *dst, const void *src, size_t pixels);
void rdps_neon_tbgr_to_565 (uint16_t *dst, const void *src, size_t pixels);
void rdps_neon_trgb_to_565 (uint16_t *dst, const void *src, size_t pixels);
void rdps_neon_copy (void *dst, const void *src, size_t bytes);
void rdps_copy_ldm8 (void *dst, const void *src, size_t n);     /* scrcopy.s */

static int ctx;          /* the module's context, or 0 */
static int tried;        /* plat_neon_start has looked */
static int entered;      /* ctx is the active one (or will be, lazily) */
static int before;       /* the context that was active before */

int
plat_neon_start (void)
{
  int fpsid, mvfr0, mvfr1, c;
  if (tried)
    return ctx != 0;
  tried = 1;
  if (_swix (XVFPSupport_Features, _IN (0) | _OUTR (0, 2), 0, &fpsid, &mvfr0, &mvfr1) != NULL)
    return 0;                   /* no VFPSupport */
  (void) fpsid; (void) mvfr0;
  if (((unsigned) mvfr1 >> 8 & 15) == 0 || ((unsigned) mvfr1 >> 12 & 15) == 0)
    return 0;                   /* VFP without NEON */
  if (_swix (XVFPSupport_CreateContext, _INR (0, 3) | _OUT (0),
             CTX_USER_MODE, 32, 0, 0, &c) != NULL || c == 0)
    return 0;
  ctx = c;
  return 1;
}

int
plat_neon_enter (void)
{
  int old;
  if (!ctx || entered)
    return -1;                  /* (not nested: a nested leave would switch out early) */
  if (_swix (XVFPSupport_ChangeContext, _INR (0, 1) | _OUT (0), ctx, CHANGE_LAZY, &old) != NULL)
    return -1;
  before = old;
  entered = 1;
  return 0;
}

void
plat_neon_leave (void)
{
  if (!entered)
    return;
  _swix (XVFPSupport_ChangeContext, _INR (0, 1), before, CHANGE_NOW);
  entered = 0;
}

/* The context is destroyed, not switched back from: by now the one that
   was active before may be gone, or in application space that isn't paged
   in. Destroying ours activates the null context if ours is the active
   one, and changes nothing otherwise (VFPSupport_DestroyContext). */
static void
forget (void)
{
  if (ctx)
    _swix (XVFPSupport_DestroyContext, _INR (0, 1), ctx, 0);
  ctx = 0;
  entered = 0;
  tried = 0;
}

void
plat_neon_reset (void)
{
  if (entered)
    forget ();                  /* made again by the next plat_neon_start */
}

void
plat_neon_stop (void)
{
  if (entered)
    forget ();
  else if (ctx)
    {
      _swix (XVFPSupport_DestroyContext, _INR (0, 1), ctx, 0);
      ctx = 0;
      tried = 0;
    }
}

int
plat_neon_convert (int rgb, int client_bpp, void *dst, const void *src, unsigned pixels)
{
  if (!entered || (pixels & 7))
    return -1;
  if (client_bpp == 32)
    (rgb ? rdps_neon_trgb_to_32 : rdps_neon_tbgr_to_32) (dst, src, pixels);
  else if (client_bpp == 16)
    (rgb ? rdps_neon_trgb_to_565 : rdps_neon_tbgr_to_565) (dst, src, pixels);
  else
    return -1;
  return 0;
}

void
plat_neon_copy (void *dst, const void *src, unsigned bytes)
{
  if (entered)
    rdps_neon_copy (dst, src, bytes & ~63u);
  else
    rdps_copy_ldm8 (dst, src, bytes & ~63u);
}
