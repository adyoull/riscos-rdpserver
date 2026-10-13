/* RDPServer module: its printf/scanf formatting (src/mod/fmt.c) against
   the C library's, on Linux (32-bit, as RISC OS: -m32).
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "mod/fmt.h"

static int checks, fails;

static int
mine (char *b, size_t n, const char *f, ...)
{
  va_list ap;
  int r;
  va_start (ap, f);
  r = rdps_vformat (b, n, f, ap);
  va_end (ap);
  return r;
}

#define SAME(size, ...) do { \
    char a[256], b[256]; int ra, rb; \
    memset (a, 'Z', sizeof a); memset (b, 'Z', sizeof b); \
    ra = snprintf (a, size, __VA_ARGS__); rb = mine (b, size, __VA_ARGS__); \
    checks++; \
    if (ra != rb || memcmp (a, b, size) != 0) { fails++; \
      printf ("FAIL %s: libc %d \"%s\", ours %d \"%s\"\n", #__VA_ARGS__, ra, a, rb, b); } \
  } while (0)

static int
scan (const char *s, const char *f, ...)
{
  va_list ap;
  int r;
  va_start (ap, f);
  r = rdps_vscan (s, f, ap);
  va_end (ap);
  return r;
}

int
main (void)
{
  void *p = (void *) 0x1234abcd;
  SAME (256, "plain");
  SAME (256, "%d %i %u", 0, -5, 3000000000u);
  SAME (256, "%d %d", INT_MIN, INT_MAX);
  SAME (256, "%5d|%-5d|%05d|%+d|% d|%.3d|%8.3d|%-8.3d|", 42, 42, 42, 42, 42, 7, 7, -7);
  SAME (256, "%x %X %#x %#X %08x %#08x %o %#o %#o", 255u, 255u, 255u, 0xabcu, 0x1fu, 0x1fu, 8u, 8u, 0u);
  SAME (256, "%#x %.0d|%.0x|", 0u, 0, 0u);
  SAME (256, "%ld %lu %lx", -123456789L, 4000000000UL, 0xdeadbeefUL);
  SAME (256, "%lld %llu %llx %llX", -1234567890123LL, 18446744073709551615ULL, 0x123456789abcdefULL, 0xfedcba9876543210ULL);
  SAME (256, "%hd %hu %hhd %hhu %hhx", (short) -3, (unsigned short) 65535, (signed char) -1, (unsigned char) 200, (unsigned char) 0xab);
  SAME (256, "%zu %zd %zx", (size_t) 77, (ssize_t) -9, (size_t) 0xff);
  SAME (256, "%s|%10s|%-10s|%.2s|%10.2s|%-*s|%*s|", "abc", "abc", "abc", "abc", "abc", 6, "ab", 4, "x");
  SAME (256, "%c%c%3c%-3c|", 'a', 'b', 'c', 'd');
  SAME (256, "100%% %p %s", p, "ok");
  SAME (256, "%02X%02X", 0x0a, 0xff);
  SAME (256, "%.*s|%.*d", 3, "abcdef", 4, 12);
  SAME (8, "%d and more", 1234567);
  SAME (1, "abc");
  SAME (256, "%-5x|%5o|", 0xabu, 9u);
  SAME (256, "\t%-*s: %s\r\n", 15, "name", "value");
  {
    char b[4];
    int r = mine (b, 0, "abc");
    checks++;
    if (r != 3) { fails++; printf ("FAIL size 0\n"); }
  }

  /* scanf */
  {
    int v = 0, n;
    char x[64], y[64], z[96];
    n = scan ("ff", "%2x", &v);
    checks++; if (n != 1 || v != 255) { fails++; printf ("FAIL %%2x: %d %d\n", n, v); }
    n = scan ("a1b2", "%2x", &v);
    checks++; if (n != 1 || v != 0xa1) { fails++; printf ("FAIL %%2x a1b2\n"); }
    n = scan ("zz", "%2x", &v);
    checks++; if (n != 0) { fails++; printf ("FAIL %%2x zz: %d\n", n); }
    n = scan ("  name  salt hash\n", "%63s %63s %95s", x, y, z);
    checks++; if (n != 3 || strcmp (x, "name") || strcmp (y, "salt") || strcmp (z, "hash"))
      { fails++; printf ("FAIL %%s: %d %s %s %s\n", n, x, y, z); }
    n = scan ("one two", "%63s %63s %95s", x, y, z);
    checks++; if (n != 2) { fails++; printf ("FAIL short line: %d\n", n); }
    n = scan ("", "%63s", x);
    checks++; if (n != -1) { fails++; printf ("FAIL empty: %d\n", n); }
    n = scan ("abcdef", "%3s", x);
    checks++; if (n != 1 || strcmp (x, "abc")) { fails++; printf ("FAIL %%3s\n"); }
    n = scan ("-42 0x1F 017", "%d %x %i", &v, &v, &v);
    checks++; if (n != 3 || v != 15) { fails++; printf ("FAIL %%d %%x %%i: %d %d\n", n, v); }
  }
  printf ("RDPServer module formatting: %d checks, %d failed\n", checks, fails);
  return fails != 0;
}
