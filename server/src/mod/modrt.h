/* RDPServer module: its run-time pieces (libc.c, module.c, header.s).
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_MODRT_H
#define RDPS_MODRT_H

#include <stddef.h>
#include <kernel.h>

/* Calls SWI swi (the X form) with R0-R9 from r, and puts R0-R9 back in r.
   Returns the error, or NULL. (header.s: OS_CallASWIR12.) */
_kernel_oserror *rdps_swi (int swi, int *r);

/* The heap, in a dynamic area. */
int rdps_heap_init (const char *name, size_t max);
void rdps_heap_final (void);
size_t rdps_heap_used (void);

/* Files held in memory (libc.c's open, fopen and stat find them). */
int rdps_memfs_set (const char *path, const char *data, size_t len);
void rdps_memfs_clear (void);

void rdps_close_all_sockets (void);
void rdps_ctype_init (void);
int rdps_snprintf_buf (char *buf, size_t size, const char *fmt, ...);

/* A line for the front end's log (module.c). tag 's': the server's own
   messages; 'x': libxrdp's log. */
void rdps_console_line (char tag, const char *text);

/* The server can't go on (exit, abort): it stops for good. */
void rdps_fatal (const char *why);

#endif
