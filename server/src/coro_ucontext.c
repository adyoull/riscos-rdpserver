/* RDPServer: coro.h on Linux (the host and QEMU test builds), with
   swapcontext.
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <ucontext.h>
#include "coro.h"

static ucontext_t outside, inside;
static void (*entry) (void *);
static void *entry_arg;
static int alive, in;

static void
trampoline (void)
{
  entry (entry_arg);
  alive = 0;
  in = 0;
  /* uc_link takes us back to outside */
}

int
co_start (void (*fn) (void *), void *arg, void *stack, size_t size)
{
  if (size < 16384 || alive)
    return -1;
  entry = fn;
  entry_arg = arg;
  getcontext (&inside);
  inside.uc_stack.ss_sp = stack;
  inside.uc_stack.ss_size = size;
  inside.uc_link = &outside;
  makecontext (&inside, trampoline, 0);
  alive = 1;
  return 0;
}

int
co_resume (void)
{
  if (!alive || in)
    return alive;
  in = 1;
  swapcontext (&outside, &inside);
  in = 0;
  return alive;
}

void
co_yield (void)
{
  if (!in)
    return;
  in = 0;
  swapcontext (&inside, &outside);
  in = 1;
}

int
co_inside (void)
{
  return in;
}
