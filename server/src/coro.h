/* RDPServer: one coroutine, the "server thread".

   The server runs on a stack of its own. Wherever it would wait (a
   socket not ready, libxrdp's connection sequence, a full send buffer)
   it calls co_yield and whoever resumed it carries on. On RISC OS that is
   the module's callback or its ByteV handler (src/mod), which resume it
   again a centisecond or so later; nothing waits with the desktop held up.

   coro_arm.s is the RISC OS version (no VFP registers: the module is
   built with -mgeneral-regs-only); coro_ucontext.c the Linux one.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_CORO_H
#define RDPS_CORO_H

#include <stddef.h>

/* Starts fn(arg) on the given stack the next time co_resume is called.
   0, or -1 if the stack is too small. */
int co_start (void (*fn) (void *), void *arg, void *stack, size_t size);

/* Runs the coroutine until it yields or fn returns. Returns 1 while it
   can be resumed again, 0 once fn has returned (or before co_start).
   Must not be called from inside it. */
int co_resume (void);

/* From inside: back to whoever called co_resume. */
void co_yield (void);

/* 1 while running inside the coroutine. */
int co_inside (void);

#endif
