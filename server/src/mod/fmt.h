/* RDPServer module: printf and scanf formatting (see fmt.c).
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_FMT_H
#define RDPS_FMT_H

#include <stdarg.h>
#include <stddef.h>

/* As vsnprintf: writes at most size bytes with the terminator, returns
   the length the whole result would have. */
int rdps_vformat (char *buf, size_t size, const char *fmt, va_list ap);

/* As vsscanf: the number of items assigned, or -1 if the input ended
   before the first. */
int rdps_vscan (const char *s, const char *fmt, va_list ap);

#endif
