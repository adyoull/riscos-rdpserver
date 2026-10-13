/* RDPServer module: printf and scanf formatting (no floating point).

   rdps_vformat does what vsnprintf does for the conversions the server,
   libxrdp and Mbed TLS use: d i u x X o c s p %, with flags - 0 + space #,
   field width and precision (numbers or *), and the length modifiers
   hh h l ll j z t. A floating-point conversion prints "?".
   rdps_vscan does sscanf's d i u x o s c [no sets] and %%, with widths and
   * (no assignment).

   Plain C with no library calls, so the host unit tests can compare it
   with the C library's (tests/test_mod_libc.c).

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "fmt.h"

typedef struct
{
  char *buf;
  size_t size, n;      /* n: characters produced (may exceed size) */
} out;

static void
put (out *o, char c)
{
  if (o->n + 1 < o->size)
    o->buf[o->n] = c;
  o->n++;
}

static void
pad (out *o, char c, int count)
{
  while (count-- > 0)
    put (o, c);
}

static size_t
slen (const char *s, int max)
{
  size_t n = 0;
  while ((max < 0 || n < (size_t) max) && s[n])
    n++;
  return n;
}

enum { F_LEFT = 1, F_ZERO = 2, F_PLUS = 4, F_SPACE = 8, F_ALT = 16 };

static void
number (out *o, unsigned long long v, int neg, unsigned base, int upper,
        int flags, int width, int prec)
{
  char digits[24];
  const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  int nd = 0, len, zeros = 0;
  char sign = 0;
  const char *prefix = "";

  while (v)
    {
      digits[nd++] = dig[v % base];
      v /= base;
    }
  if (prec < 0 && nd == 0)
    digits[nd++] = '0';               /* default precision 1 */
  if (prec >= 0 && nd < prec)
    zeros = prec - nd;
  if (neg)
    sign = '-';
  else if (flags & F_PLUS)
    sign = '+';
  else if (flags & F_SPACE)
    sign = ' ';
  if (flags & F_ALT)
    {
      if (base == 16 && nd > 0 && !(nd == 1 && digits[0] == '0'))
        prefix = upper ? "0X" : "0x";
      else if (base == 8 && zeros == 0 && (nd == 0 || digits[nd - 1] != '0'))
        zeros = 1;
    }
  len = nd + zeros + (sign ? 1 : 0) + (int) slen (prefix, -1);
  if (!(flags & F_LEFT) && !((flags & F_ZERO) && prec < 0))
    pad (o, ' ', width - len);
  if (sign)
    put (o, sign);
  while (*prefix)
    put (o, *prefix++);
  if (!(flags & F_LEFT) && (flags & F_ZERO) && prec < 0)
    pad (o, '0', width - len);
  pad (o, '0', zeros);
  while (nd > 0)
    put (o, digits[--nd]);
  if (flags & F_LEFT)
    pad (o, ' ', width - len);
}

int
rdps_vformat (char *buf, size_t size, const char *fmt, va_list ap)
{
  out o;
  o.buf = buf;
  o.size = size;
  o.n = 0;

  while (*fmt)
    {
      int flags = 0, width = 0, prec = -1, lng = 0;
      char c = *fmt++;
      if (c != '%')
        {
          put (&o, c);
          continue;
        }
      for (;; fmt++)
        {
          if (*fmt == '-') flags |= F_LEFT;
          else if (*fmt == '0') flags |= F_ZERO;
          else if (*fmt == '+') flags |= F_PLUS;
          else if (*fmt == ' ') flags |= F_SPACE;
          else if (*fmt == '#') flags |= F_ALT;
          else break;
        }
      if (*fmt == '*')
        {
          width = va_arg (ap, int);
          if (width < 0)
            {
              flags |= F_LEFT;
              width = -width;
            }
          fmt++;
        }
      else
        while (*fmt >= '0' && *fmt <= '9')
          width = width * 10 + (*fmt++ - '0');
      if (*fmt == '.')
        {
          fmt++;
          prec = 0;
          if (*fmt == '*')
            {
              prec = va_arg (ap, int);
              fmt++;
            }
          else
            while (*fmt >= '0' && *fmt <= '9')
              prec = prec * 10 + (*fmt++ - '0');
          if (prec < 0)
            prec = -1;
        }
      /* lng: -2 hh, -1 h, 0 int, 1 long, 2 long long, 3 size_t/ptrdiff/intmax */
      for (;; fmt++)
        {
          if (*fmt == 'h') lng--;
          else if (*fmt == 'l') lng++;
          else if (*fmt == 'z' || *fmt == 't') lng = 1;   /* 32-bit: as long */
          else if (*fmt == 'j') lng = 2;
          else if (*fmt == 'L') lng = 2;
          else break;
        }
      c = *fmt;
      if (c == 0)
        break;
      fmt++;
      switch (c)
        {
        case 'd': case 'i':
          {
            long long v;
            if (lng >= 2) v = va_arg (ap, long long);
            else if (lng == 1) v = va_arg (ap, long);
            else v = va_arg (ap, int);
            if (lng == -1) v = (short) v;
            else if (lng <= -2) v = (signed char) v;
            number (&o, v < 0 ? 0ULL - (unsigned long long) v : (unsigned long long) v,
                    v < 0, 10, 0, flags, width, prec);
            break;
          }
        case 'u': case 'x': case 'X': case 'o':
          {
            unsigned long long v;
            if (lng >= 2) v = va_arg (ap, unsigned long long);
            else if (lng == 1) v = va_arg (ap, unsigned long);
            else v = va_arg (ap, unsigned int);
            if (lng == -1) v = (unsigned short) v;
            else if (lng <= -2) v = (unsigned char) v;
            number (&o, v, 0, c == 'u' ? 10 : c == 'o' ? 8 : 16, c == 'X',
                    flags & ~(F_PLUS | F_SPACE), width, prec);
            break;
          }
        case 'p':
          {
            uintptr_t v = (uintptr_t) va_arg (ap, void *);
            number (&o, v, 0, 16, 0, F_ALT | (flags & F_LEFT), width, prec);
            break;
          }
        case 'c':
          if (!(flags & F_LEFT))
            pad (&o, ' ', width - 1);
          put (&o, (char) va_arg (ap, int));
          if (flags & F_LEFT)
            pad (&o, ' ', width - 1);
          break;
        case 's':
          {
            const char *s = va_arg (ap, const char *);
            size_t n, i;
            if (!s)
              s = "(null)";
            n = slen (s, prec);
            if (!(flags & F_LEFT))
              pad (&o, ' ', width - (int) n);
            for (i = 0; i < n; i++)
              put (&o, s[i]);
            if (flags & F_LEFT)
              pad (&o, ' ', width - (int) n);
            break;
          }
        case '%':
          put (&o, '%');
          break;
        case 'n':
          *va_arg (ap, int *) = (int) o.n;
          break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
          (void) va_arg (ap, double);
          put (&o, '?');
          break;
        default:
          put (&o, '%');
          put (&o, c);
          break;
        }
    }
  if (o.size > 0)
    o.buf[o.n < o.size ? o.n : o.size - 1] = 0;
  return (int) o.n;
}

/* --- scanf ---------------------------------------------------------------- */

static int
is_space (char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int
digit_value (char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 99;
}

int
rdps_vscan (const char *s, const char *fmt, va_list ap)
{
  int assigned = 0;
  const char *start = s;

  while (*fmt)
    {
      int width = 0, skip = 0, lng = 0;
      char c = *fmt++;
      if (is_space (c))
        {
          while (is_space (*s))
            s++;
          continue;
        }
      if (c != '%')
        {
          if (*s != c)
            break;
          s++;
          continue;
        }
      if (*fmt == '*')
        {
          skip = 1;
          fmt++;
        }
      while (*fmt >= '0' && *fmt <= '9')
        width = width * 10 + (*fmt++ - '0');
      for (;; fmt++)
        {
          if (*fmt == 'h') lng--;
          else if (*fmt == 'l') lng++;
          else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') lng = 1;
          else break;
        }
      c = *fmt++;
      if (c == 0)
        break;
      if (c == '%')
        {
          while (is_space (*s))
            s++;
          if (*s != '%')
            break;
          s++;
          continue;
        }
      if (c == 'n')
        {
          if (!skip)
            *va_arg (ap, int *) = (int) (s - start);
          continue;
        }
      if (c != 'c')
        while (is_space (*s))
          s++;
      if (*s == 0)
        return assigned == 0 ? -1 : assigned;
      if (c == 's' || c == 'c')
        {
          char *d = skip ? NULL : va_arg (ap, char *);
          int max = width ? width : (c == 'c' ? 1 : 0x7fffffff), n = 0;
          while (*s && n < max && (c == 'c' || !is_space (*s)))
            {
              if (d)
                d[n] = *s;
              n++;
              s++;
            }
          if (c == 'c' && n < max)
            break;
          if (d && c == 's')
            d[n] = 0;
          if (!skip)
            assigned++;
          continue;
        }
      if (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X' || c == 'o')
        {
          unsigned base = (c == 'x' || c == 'X') ? 16 : c == 'o' ? 8 : 10;
          int neg = 0, nd = 0, max = width ? width : 0x7fffffff, used = 0;
          unsigned long long v = 0;
          if ((*s == '-' || *s == '+') && used < max)
            {
              neg = *s == '-';
              s++;
              used++;
            }
          if (c == 'i' && *s == '0' && used < max)
            {
              base = 8;
              if ((s[1] == 'x' || s[1] == 'X') && used + 2 < max && digit_value (s[2]) < 16)
                {
                  base = 16;
                  s += 2;
                  used += 2;
                }
            }
          else if (base == 16 && *s == '0' && (s[1] == 'x' || s[1] == 'X')
                   && used + 2 < max && digit_value (s[2]) < 16)
            {
              s += 2;
              used += 2;
            }
          while (used < max && digit_value (*s) < (int) base)
            {
              v = v * base + (unsigned) digit_value (*s++);
              used++;
              nd++;
            }
          if (nd == 0)
            break;
          if (neg)
            v = 0ULL - v;
          if (!skip)
            {
              if (lng >= 2)
                *va_arg (ap, long long *) = (long long) v;
              else if (lng == 1)
                *va_arg (ap, long *) = (long) v;
              else if (lng == -1)
                *va_arg (ap, short *) = (short) v;
              else if (lng <= -2)
                *va_arg (ap, signed char *) = (signed char) v;
              else
                *va_arg (ap, int *) = (int) v;
              assigned++;
            }
          continue;
        }
      break;   /* not supported */
    }
  return assigned;
}
