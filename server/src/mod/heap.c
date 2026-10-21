/* RDPServer module: memory for the module's C library (libc.c): an
   OS_Heap in a dynamic area of our own (PRM OS_DynamicArea 0 p318,
   OS_Heap p393-397), grown as needed; malloc and the rest; and the block
   kept back for running out (platform.h, plat_memory_*).

   Compiled with -fno-builtin, as libc.c is, so that the compiler doesn't
   turn malloc and memset in calloc into a call to calloc.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>

#include "modrt.h"
#include "../platform.h"

static int heap_area = -1;          /* dynamic area number */
static char *heap;                  /* its base: the OS_Heap */
static size_t heap_size, heap_max;
/* Out of memory: a block kept back during a session, given
   up when a claim fails so that the claim, and the small ones after it
   (libxrdp doesn't look for NULL from its stream claims), still succeed;
   the server then ends the session (plat_memory_short). */
static void *reserve;
#define RESERVE_FOR (256 * 1024)  /* claims under this size only */
static int mem_short;
static int grow_refused;        /* RISC OS wouldn't make the area bigger */

#define DA_FLAGS 0x80               /* user read/write, not draggable */
#define HEAP_START (1024 * 1024)
#define HEAP_STEP (256 * 1024)

int
rdps_heap_init (const char *name, size_t max)
{
  int r[10];
  if (heap)
    return 0;
  memset (r, 0, sizeof r);
  r[0] = 0;
  r[1] = -1;
  r[2] = HEAP_START;
  r[3] = -1;
  r[4] = DA_FLAGS;
  r[5] = (int) max;
  r[6] = 0;
  r[7] = 0;
  r[8] = (int) name;
  if (rdps_swi (OS_DynamicArea, r))
    return -1;
  heap_area = r[1];
  heap = (char *) r[3];
  heap_size = HEAP_START;
  heap_max = (size_t) r[5];
  memset (r, 0, sizeof r);
  r[0] = 0;                         /* OS_Heap 0: initialise */
  r[1] = (int) heap;
  r[3] = (int) heap_size;
  if (rdps_swi (OS_Heap, r))
    {
      rdps_heap_final ();
      return -1;
    }
  return 0;
}

void
rdps_heap_final (void)
{
  int r[10];
  if (heap_area < 0)
    return;
  memset (r, 0, sizeof r);
  r[0] = 1;                         /* OS_DynamicArea 1: remove */
  r[1] = heap_area;
  rdps_swi (OS_DynamicArea, r);
  heap_area = -1;
  heap = NULL;
  heap_size = 0;
}

size_t
rdps_heap_used (void)
{
  return heap_size;
}

/* Makes the area (and the heap in it) at least `more' bytes bigger. */
static int
heap_grow (size_t more)
{
  int r[10];
  size_t step = (more + HEAP_STEP - 1) / HEAP_STEP * HEAP_STEP;
  if (!heap || heap_size + step > heap_max)
    return -1;
  memset (r, 0, sizeof r);
  r[0] = heap_area;
  r[1] = (int) step;
  if (rdps_swi (OS_ChangeDynamicArea, r))
    {
      grow_refused = 1;
      return -1;
    }
  step = (size_t) r[1];             /* what it actually grew by */
  grow_refused = 0;
  memset (r, 0, sizeof r);
  r[0] = 5;                         /* OS_Heap 5: change the heap's size */
  r[1] = (int) heap;
  r[3] = (int) step;
  rdps_swi (OS_Heap, r);
  heap_size += step;
  return 0;
}

/* Each block starts with 8 bytes of our own: the size asked for (so
   realloc knows it, without relying on what OS_Heap 6 counts), and
   padding to keep the pointer 8-byte aligned. */
#define BLOCK_HEAD 8

static void *heap_claim_once (size_t n);

static void
heap_release (void *p)
{
  int r[10];
  memset (r, 0, sizeof r);
  r[0] = 3;                         /* OS_Heap 3: release */
  r[1] = (int) heap;
  r[2] = (int) ((char *) p - BLOCK_HEAD);
  rdps_swi (OS_Heap, r);
}

static void *
heap_claim (size_t n)
{
  void *p = heap_claim_once (n);
  /* Only small claims may have the block kept back: libxrdp's streams,
     which aren't checked. The big ones (the copy of the screen, the
     login page) are the server's own, which are; letting them have it
     would leave nothing for the small ones after. */
  if (!p && reserve && n < RESERVE_FOR)
    {
      heap_release (reserve);
      reserve = NULL;
      mem_short = 1;
      p = heap_claim_once (n);
    }
  if (!p)
    mem_short = 1;
  return p;
}

int
plat_memory_reserve (unsigned bytes)
{
  if (!reserve)
    reserve = heap_claim_once (bytes);
  return reserve ? 0 : -1;
}

int
plat_memory_short (void)
{
  int was = mem_short;
  mem_short = 0;
  return was;
}

void
plat_memory_info (unsigned *used_kb, unsigned *max_kb, int *refused)
{
  *used_kb = (unsigned) (heap_size / 1024);
  *max_kb = (unsigned) (heap_max / 1024);
  *refused = grow_refused;
}

static void *
heap_claim_once (size_t n)
{
  int r[10], tries;
  if (!heap || n > 0x7fff0000u)
    return NULL;
  for (tries = 0; tries < 2; tries++)
    {
      memset (r, 0, sizeof r);
      r[0] = 2;                     /* OS_Heap 2: claim */
      r[1] = (int) heap;
      r[3] = (int) (n + BLOCK_HEAD);
      if (rdps_swi (OS_Heap, r) == NULL && r[2])
        {
          size_t *b = (size_t *) r[2];
          b[0] = n;
          return (char *) b + BLOCK_HEAD;
        }
      if (heap_grow (n + BLOCK_HEAD + 64) != 0)
        break;
    }
  errno = ENOMEM;
  return NULL;
}

static size_t
block_size (const void *p)
{
  return *(const size_t *) ((const char *) p - BLOCK_HEAD);
}

void *
malloc (size_t n)
{
  return heap_claim (n);
}

void *
calloc (size_t a, size_t b)
{
  void *p;
  if (b && a > (size_t) -1 / b)
    return NULL;
  p = heap_claim (a * b);
  if (p)
    memset (p, 0, a * b);
  return p;
}

void
free (void *p)
{
  if (!p || !heap)
    return;
  heap_release (p);
}

void *
realloc (void *p, size_t n)
{
  size_t have;
  void *q;
  if (!p)
    return malloc (n);
  if (n == 0)
    {
      free (p);
      return NULL;
    }
  have = block_size (p);
  if (have >= n)
    return p;
  q = malloc (n);
  if (!q)
    return NULL;
  memcpy (q, p, have);
  free (p);
  return q;
}

char *
strdup (const char *s)
{
  size_t n = strlen (s) + 1;
  char *d = malloc (n);
  if (d)
    memcpy (d, s, n);
  return d;
}
