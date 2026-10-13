/* RDPServer module: the parts of a C library the server, libxrdp and
   Mbed TLS use, for code that runs in a module (no UnixLib: its memory
   is the application's, which isn't paged in when the module runs).

   Compiled against UnixLib's headers, so the declarations (and types
   such as struct sockaddr_in and fd_set) are the ones the rest of the
   code was compiled with.

   - memory: an OS_Heap in a dynamic area of our own (PRM OS_DynamicArea 0
     p318, OS_Heap p393-397), grown as needed;
   - strings, ctype (ASCII), printf/scanf (fmt.c);
   - files: read-only, from memory: the front end hands the module the
     files libxrdp reads (xrdp.ini, rsakeys.ini, logins) with
     rdps_memfs_set, so the module never calls FileSwitch from a callback
     or from inside the Wimp's error box; /dev/urandom is CryptRandom;
   - stdout/stderr: lines to the message list the front end reads;
   - sockets: the Internet module's SWIs directly (as UnixLib's
     incl-local/sys/socket.h does), errno = error number & &FF;
   - time: OS_Word 14,3 (UTC; the log's times are UTC);
   - waits (usleep): the server thread's (srvthread.h);
   - exit/abort: the server thread stops for good (rdps_fatal).

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <kernel.h>
#include <swis.h>

#include "fmt.h"
#include "modrt.h"
#include "../srvthread.h"

int errno;

/* --- SWIs ----------------------------------------------------------------- */

_kernel_oserror *
_swix (int swi_no, unsigned int mask, ...)
{
  int r[10];
  int i;
  _kernel_oserror *e;
  va_list ap;

  memset (r, 0, sizeof r);
  va_start (ap, mask);
  for (i = 0; i < 10; i++)
    if (mask & (1u << i))
      r[i] = va_arg (ap, int);
  e = rdps_swi (swi_no, r);
  if (!e)
    {
      for (i = 0; i < 10; i++)
        if (mask & (1u << (31 - i)))
          *va_arg (ap, int *) = r[i];
      if (mask & (1u << 21))
        *va_arg (ap, int *) = 0;   /* flags: not available */
    }
  va_end (ap);
  return e;
}

static int
set_errno_from (const _kernel_oserror *e)
{
  errno = e ? (e->errnum & 0xff) : 0;
  if (errno == 0)
    errno = EIO;
  return -1;
}

/* --- memory --------------------------------------------------------------- */

static int heap_area = -1;          /* dynamic area number */
static char *heap;                  /* its base: the OS_Heap */
static size_t heap_size, heap_max;

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
    return -1;
  step = (size_t) r[1];             /* what it actually grew by */
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

static void *
heap_claim (size_t n)
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
  int r[10];
  if (!p || !heap)
    return;
  memset (r, 0, sizeof r);
  r[0] = 3;                         /* OS_Heap 3: release */
  r[1] = (int) heap;
  r[2] = (int) ((char *) p - BLOCK_HEAD);
  rdps_swi (OS_Heap, r);
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

/* --- strings -------------------------------------------------------------- */
/* (This file is compiled with -fno-builtin and
   -fno-tree-loop-distribute-patterns, so these loops stay loops.) */

void *
memcpy (void *d, const void *s, size_t n)
{
  unsigned char *dp = d;
  const unsigned char *sp = s;
  if ((((uintptr_t) dp | (uintptr_t) sp) & 3) == 0)
    {
      while (n >= 16)
        {
          ((uint32_t *) dp)[0] = ((const uint32_t *) sp)[0];
          ((uint32_t *) dp)[1] = ((const uint32_t *) sp)[1];
          ((uint32_t *) dp)[2] = ((const uint32_t *) sp)[2];
          ((uint32_t *) dp)[3] = ((const uint32_t *) sp)[3];
          dp += 16;
          sp += 16;
          n -= 16;
        }
      while (n >= 4)
        {
          *(uint32_t *) dp = *(const uint32_t *) sp;
          dp += 4;
          sp += 4;
          n -= 4;
        }
    }
  while (n--)
    *dp++ = *sp++;
  return d;
}

void *
memmove (void *d, const void *s, size_t n)
{
  unsigned char *dp = d;
  const unsigned char *sp = s;
  if (dp == sp || n == 0)
    return d;
  if (dp < sp || dp >= sp + n)
    return memcpy (d, s, n);
  while (n--)
    dp[n] = sp[n];
  return d;
}

void *
memset (void *d, int c, size_t n)
{
  unsigned char *dp = d;
  uint32_t w = (unsigned char) c;
  w |= w << 8;
  w |= w << 16;
  while (n && ((uintptr_t) dp & 3))
    {
      *dp++ = (unsigned char) c;
      n--;
    }
  while (n >= 4)
    {
      *(uint32_t *) dp = w;
      dp += 4;
      n -= 4;
    }
  while (n--)
    *dp++ = (unsigned char) c;
  return d;
}

int
memcmp (const void *a, const void *b, size_t n)
{
  const unsigned char *x = a, *y = b;
  for (; n; n--, x++, y++)
    if (*x != *y)
      return *x - *y;
  return 0;
}

void *
memchr (const void *s, int c, size_t n)
{
  const unsigned char *p = s;
  for (; n; n--, p++)
    if (*p == (unsigned char) c)
      return (void *) p;
  return NULL;
}

size_t
strlen (const char *s)
{
  const char *p = s;
  while (*p)
    p++;
  return (size_t) (p - s);
}

int
strcmp (const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return (unsigned char) *a - (unsigned char) *b;
}

int
strncmp (const char *a, const char *b, size_t n)
{
  for (; n; n--, a++, b++)
    {
      if (*a != *b)
        return (unsigned char) *a - (unsigned char) *b;
      if (!*a)
        return 0;
    }
  return 0;
}

static int
lower (int c)
{
  return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

int
strcasecmp (const char *a, const char *b)
{
  while (*a && lower ((unsigned char) *a) == lower ((unsigned char) *b))
    a++, b++;
  return lower ((unsigned char) *a) - lower ((unsigned char) *b);
}

int
strncasecmp (const char *a, const char *b, size_t n)
{
  for (; n; n--, a++, b++)
    {
      int x = lower ((unsigned char) *a), y = lower ((unsigned char) *b);
      if (x != y)
        return x - y;
      if (!x)
        return 0;
    }
  return 0;
}

char *
strcpy (char *d, const char *s)
{
  char *r = d;
  while ((*d++ = *s++))
    ;
  return r;
}

char *
strncpy (char *d, const char *s, size_t n)
{
  size_t i = 0;
  for (; i < n && s[i]; i++)
    d[i] = s[i];
  for (; i < n; i++)
    d[i] = 0;
  return d;
}

char *
strcat (char *d, const char *s)
{
  strcpy (d + strlen (d), s);
  return d;
}

char *
strncat (char *d, const char *s, size_t n)
{
  char *e = d + strlen (d);
  while (n-- && *s)
    *e++ = *s++;
  *e = 0;
  return d;
}

char *
strchr (const char *s, int c)
{
  for (;; s++)
    {
      if (*s == (char) c)
        return (char *) s;
      if (!*s)
        return NULL;
    }
}

char *
strrchr (const char *s, int c)
{
  const char *r = NULL;
  for (;; s++)
    {
      if (*s == (char) c)
        r = s;
      if (!*s)
        return (char *) r;
    }
}

char *
strstr (const char *h, const char *n)
{
  size_t ln = strlen (n);
  if (!ln)
    return (char *) h;
  for (; *h; h++)
    if (*h == *n && strncmp (h, n, ln) == 0)
      return (char *) h;
  return NULL;
}

char *
strtok (char *s, const char *delim)
{
  static char *next;
  char *start;
  if (s)
    next = s;
  if (!next)
    return NULL;
  while (*next && strchr (delim, *next))
    next++;
  if (!*next)
    {
      next = NULL;
      return NULL;
    }
  start = next;
  while (*next && !strchr (delim, *next))
    next++;
  if (*next)
    *next++ = 0;
  else
    next = NULL;
  return start;
}

long
strtol (const char *s, char **end, int base)
{
  const char *p = s;
  unsigned long v = 0;
  int neg = 0, any = 0;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;
  if (*p == '-' || *p == '+')
    neg = *p++ == '-';
  if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
      p += 2;
      base = 16;
    }
  else if (base == 0)
    base = *p == '0' ? 8 : 10;
  for (;; p++)
    {
      int d;
      if (*p >= '0' && *p <= '9') d = *p - '0';
      else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
      else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
      else break;
      if (d >= base)
        break;
      v = v * (unsigned) base + (unsigned) d;
      any = 1;
    }
  if (end)
    *end = (char *) (any ? p : s);
  return neg ? -(long) v : (long) v;
}

int
atoi (const char *s)
{
  return (int) strtol (s, NULL, 10);
}

char *
strerror (int e)
{
  static char buf[24];
  switch (e)
    {
    case 0: return "no error";
    case ENOENT: return "no such file";
    case ENOMEM: return "out of memory";
    case EACCES: return "permission denied";
    case EINVAL: return "invalid argument";
    case EPIPE: return "broken pipe";
    case EWOULDBLOCK: return "operation would block";
    case EINPROGRESS: return "operation in progress";
    case EADDRINUSE: return "address already in use";
    case EADDRNOTAVAIL: return "address not available";
    case ECONNRESET: return "connection reset by peer";
    case ECONNREFUSED: return "connection refused";
    case ENOTCONN: return "socket not connected";
    case ETIMEDOUT: return "timed out";
    case EAFNOSUPPORT: return "address family not supported";
    default:
      rdps_snprintf_buf (buf, sizeof buf, "error %d", e);
      return buf;
    }
}

/* ctype: ASCII. The tables are indexed from -1 (EOF). */
#define C_CTRL 1
#define C_UP 2
#define C_LOW 4
#define C_ALPHA 8
#define C_PUNCT 16
#define C_WHITE 32
#define C_DIGIT 64
#define C_XDIG 128
static unsigned char ctype_tab[257];
static short lower_tab[257], upper_tab[257];
const unsigned char *const __ctype = ctype_tab + 1;
const short *const __ctype_lower = lower_tab + 1;
const short *const __ctype_upper = upper_tab + 1;

void
rdps_ctype_init (void)
{
  int c;
  lower_tab[0] = upper_tab[0] = -1;
  for (c = 0; c < 256; c++)
    {
      unsigned char f = 0;
      if (c < 32 || c == 127) f |= C_CTRL;
      if (c == ' ' || (c >= 9 && c <= 13)) f |= C_WHITE;
      if (c >= 'A' && c <= 'Z') f |= C_UP | C_ALPHA;
      if (c >= 'a' && c <= 'z') f |= C_LOW | C_ALPHA;
      if (c >= '0' && c <= '9') f |= C_DIGIT | C_XDIG;
      if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) f |= C_XDIG;
      if (c > 32 && c < 127 && !(f & (C_ALPHA | C_DIGIT))) f |= C_PUNCT;
      ctype_tab[c + 1] = f;
      lower_tab[c + 1] = (short) ((c >= 'A' && c <= 'Z') ? c + 32 : c);
      upper_tab[c + 1] = (short) ((c >= 'a' && c <= 'z') ? c - 32 : c);
    }
}

/* --- printf --------------------------------------------------------------- */

int
vsnprintf (char *buf, size_t size, const char *fmt, va_list ap)
{
  return rdps_vformat (buf, size, fmt, ap);
}

int
snprintf (char *buf, size_t size, const char *fmt, ...)
{
  va_list ap;
  int n;
  va_start (ap, fmt);
  n = rdps_vformat (buf, size, fmt, ap);
  va_end (ap);
  return n;
}

int
rdps_snprintf_buf (char *buf, size_t size, const char *fmt, ...)
{
  va_list ap;
  int n;
  va_start (ap, fmt);
  n = rdps_vformat (buf, size, fmt, ap);
  va_end (ap);
  return n;
}

int
vsprintf (char *buf, const char *fmt, va_list ap)
{
  return rdps_vformat (buf, 0x7fffffff, fmt, ap);
}

int
sprintf (char *buf, const char *fmt, ...)
{
  va_list ap;
  int n;
  va_start (ap, fmt);
  n = rdps_vformat (buf, 0x7fffffff, fmt, ap);
  va_end (ap);
  return n;
}

int
sscanf (const char *s, const char *fmt, ...)
{
  va_list ap;
  int n;
  va_start (ap, fmt);
  n = rdps_vscan (s, fmt, ap);
  va_end (ap);
  return n;
}

/* --- FILE: the console (lines to the message list) and memory files ------- */

#define MFILE_MAGIC 0x4c49464d     /* "MFIL" */
typedef struct
{
  unsigned magic;
  int console;                     /* 1: stdout/stderr */
  const char *data;                /* memory file */
  size_t len, pos;
  size_t ll;                       /* console: characters in line */
  char line[200];
} mfile;

static mfile out_file = { MFILE_MAGIC, 1, NULL, 0, 0, 0, { 0 } };
static mfile err_file = { MFILE_MAGIC, 1, NULL, 0, 0, 0, { 0 } };
FILE *__stdout = (FILE *) &out_file;
FILE *__stderr = (FILE *) &err_file;
FILE *__stdin = NULL;

static void
console_flush (mfile *f)
{
  if (f->ll == 0)
    return;
  f->line[f->ll] = 0;
  rdps_console_line ('x', f->line);
  f->ll = 0;
}

static void
console_put (mfile *f, char c)
{
  if (c == '\r')
    return;
  if (c == '\n')
    {
      console_flush (f);
      return;
    }
  if (f->ll + 1 >= sizeof f->line)
    console_flush (f);
  f->line[f->ll++] = c;
}

static mfile *
as_mfile (FILE *f)
{
  mfile *m = (mfile *) f;
  return m && m->magic == MFILE_MAGIC ? m : NULL;
}

int
vfprintf (FILE *f, const char *fmt, va_list ap)
{
  char buf[512];
  mfile *m = as_mfile (f);
  int n = rdps_vformat (buf, sizeof buf, fmt, ap), i;
  if (!m || !m->console)
    return -1;
  for (i = 0; buf[i]; i++)
    console_put (m, buf[i]);
  return n;
}

int
fprintf (FILE *f, const char *fmt, ...)
{
  va_list ap;
  int n;
  va_start (ap, fmt);
  n = vfprintf (f, fmt, ap);
  va_end (ap);
  return n;
}

int
printf (const char *fmt, ...)
{
  va_list ap;
  int n;
  va_start (ap, fmt);
  n = vfprintf (stdout, fmt, ap);
  va_end (ap);
  return n;
}

int
fputc (int c, FILE *f)
{
  mfile *m = as_mfile (f);
  if (!m || !m->console)
    return EOF;
  console_put (m, (char) c);
  return (unsigned char) c;
}

size_t
fwrite (const void *p, size_t size, size_t n, FILE *f)
{
  mfile *m = as_mfile (f);
  const char *c = p;
  size_t i;
  if (!m || !m->console)
    return 0;
  for (i = 0; i < size * n; i++)
    console_put (m, c[i]);
  return n;
}

int
fflush (FILE *f)
{
  (void) f;
  return 0;
}

/* Memory files: the paths the front end gave rdps_memfs_set. */
#define MEMFS_MAX 8
static struct { char path[256]; char *data; size_t len; } memfs[MEMFS_MAX];

int
rdps_memfs_set (const char *path, const char *data, size_t len)
{
  int i, slot = -1;
  if (strlen (path) >= sizeof memfs[0].path)
    return -1;
  for (i = 0; i < MEMFS_MAX; i++)
    if (memfs[i].path[0] && strcmp (memfs[i].path, path) == 0)
      slot = i;
  if (slot < 0)
    for (i = 0; i < MEMFS_MAX && slot < 0; i++)
      if (!memfs[i].path[0])
        slot = i;
  if (slot < 0)
    return -1;
  free (memfs[slot].data);
  memfs[slot].data = NULL;
  memfs[slot].path[0] = 0;
  if (data)
    {
      memfs[slot].data = malloc (len + 1);
      if (!memfs[slot].data)
        return -1;
      memcpy (memfs[slot].data, data, len);
      memfs[slot].data[len] = 0;
      memfs[slot].len = len;
      strcpy (memfs[slot].path, path);
    }
  return 0;
}

void
rdps_memfs_clear (void)
{
  int i;
  for (i = 0; i < MEMFS_MAX; i++)
    {
      free (memfs[i].data);
      memfs[i].data = NULL;
      memfs[i].path[0] = 0;
    }
}

static int
memfs_find (const char *path)
{
  int i;
  for (i = 0; i < MEMFS_MAX; i++)
    if (memfs[i].path[0] && strcmp (memfs[i].path, path) == 0)
      return i;
  return -1;
}

FILE *
fopen (const char *path, const char *mode)
{
  int i = memfs_find (path);
  mfile *m;
  if (i < 0 || mode[0] != 'r' || strchr (mode, '+'))
    {
      errno = i < 0 ? ENOENT : EACCES;
      return NULL;
    }
  m = calloc (1, sizeof *m);
  if (!m)
    return NULL;
  m->magic = MFILE_MAGIC;
  m->data = memfs[i].data;
  m->len = memfs[i].len;
  return (FILE *) m;
}

char *
fgets (char *buf, int size, FILE *f)
{
  mfile *m = as_mfile (f);
  int n = 0;
  if (!m || m->console || size <= 0 || m->pos >= m->len)
    return NULL;
  while (n + 1 < size && m->pos < m->len)
    {
      char c = m->data[m->pos++];
      buf[n++] = c;
      if (c == '\n')
        break;
    }
  buf[n] = 0;
  return buf;
}

int
fclose (FILE *f)
{
  mfile *m = as_mfile (f);
  if (!m || m->console)
    return 0;
  m->magic = 0;
  free (m);
  return 0;
}

/* --- file descriptors ----------------------------------------------------- */
/* Sockets are the Internet module's own numbers (0..255); memory files are
   FD_MEM + slot; /dev/urandom is FD_RANDOM; 1 and 2 are the console. */
#define FD_MEM 0x4000
#define FD_RANDOM 0x5000
#define FDS_OPEN 16
static struct { int used, slot; size_t pos; } fds[FDS_OPEN];
static unsigned char sockets_open[256 / 8];   /* to close them all at the end */

int
open (const char *path, int flags, ...)
{
  int i, slot;
  if (strcmp (path, "/dev/urandom") == 0 || strcmp (path, "/dev/random") == 0)
    return FD_RANDOM;
  if ((flags & (O_WRONLY | O_RDWR | O_CREAT)) != 0)
    {
      errno = EACCES;
      return -1;
    }
  slot = memfs_find (path);
  if (slot < 0)
    {
      errno = ENOENT;
      return -1;
    }
  for (i = 0; i < FDS_OPEN; i++)
    if (!fds[i].used)
      {
        fds[i].used = 1;
        fds[i].slot = slot;
        fds[i].pos = 0;
        return FD_MEM + i;
      }
  errno = EMFILE;
  return -1;
}

ssize_t
read (int fd, void *buf, size_t n)
{
  if (fd == FD_RANDOM)
    {
      int r[10];
      memset (r, 0, sizeof r);
      r[0] = (int) buf;
      r[1] = (int) n;
      if (rdps_swi (CryptRandom_Block, r))
        return set_errno_from (NULL);
      return (ssize_t) n;
    }
  if (fd >= FD_MEM && fd < FD_MEM + FDS_OPEN && fds[fd - FD_MEM].used)
    {
      int i = fd - FD_MEM;
      size_t len = memfs[fds[i].slot].len, left;
      left = fds[i].pos < len ? len - fds[i].pos : 0;
      if (n > left)
        n = left;
      memcpy (buf, memfs[fds[i].slot].data + fds[i].pos, n);
      fds[i].pos += n;
      return (ssize_t) n;
    }
  errno = EBADF;
  return -1;
}

static int is_open_socket (int fd);

ssize_t
write (int fd, const void *buf, size_t n)
{
  if ((fd == 1 || fd == 2) && !is_open_socket (fd))
    {
      fwrite (buf, 1, n, fd == 1 ? stdout : stderr);
      return (ssize_t) n;
    }
  errno = EBADF;
  return -1;
}

off_t
lseek (int fd, off_t off, int whence)
{
  if (fd >= FD_MEM && fd < FD_MEM + FDS_OPEN && fds[fd - FD_MEM].used)
    {
      int i = fd - FD_MEM;
      size_t len = memfs[fds[i].slot].len;
      long p = whence == SEEK_SET ? (long) off
               : whence == SEEK_CUR ? (long) fds[i].pos + (long) off
               : (long) len + (long) off;
      if (p < 0)
        {
          errno = EINVAL;
          return -1;
        }
      fds[i].pos = (size_t) p;
      return (off_t) p;
    }
  errno = ESPIPE;
  return -1;
}

static int
socket_fd (int fd)
{
  return fd >= 0 && fd < 256;
}

static void
mark_socket (int s, int open)
{
  if (!socket_fd (s))
    return;
  if (open)
    sockets_open[s / 8] |= (unsigned char) (1 << (s % 8));
  else
    sockets_open[s / 8] &= (unsigned char) ~(1 << (s % 8));
}

static int
is_open_socket (int fd)
{
  return fd >= 0 && fd < 256 && (sockets_open[fd / 8] & (1 << (fd % 8)));
}

/* Socket numbers are the Internet module's, shared by every program and
   starting at 0, so 1 and 2 may well be ours. */
int
close (int fd)
{
  if (fd == FD_RANDOM)
    return 0;
  if (fd >= FD_MEM && fd < FD_MEM + FDS_OPEN)
    {
      fds[fd - FD_MEM].used = 0;
      return 0;
    }
  if (is_open_socket (fd))
    {
      /* only our own: the number of a socket we don't have open could be
         another program's */
      int r[10];
      memset (r, 0, sizeof r);
      r[0] = fd;
      mark_socket (fd, 0);
      if (rdps_swi (Socket_Close, r))
        return set_errno_from (NULL);
      return 0;
    }
  if (fd == 1 || fd == 2)
    return 0;
  errno = EBADF;
  return -1;
}

/* Closes every socket the module still has open (the module is going). */
void
rdps_close_all_sockets (void)
{
  int s;
  for (s = 0; s < 256; s++)
    if (sockets_open[s / 8] & (1 << (s % 8)))
      close (s);
}

int
stat (const char *path, struct stat *st)
{
  int i = memfs_find (path);
  if (i < 0)
    {
      errno = ENOENT;
      return -1;
    }
  memset (st, 0, sizeof *st);
  st->st_size = (off_t) memfs[i].len;
  st->st_mode = S_IFREG | 0444;
  return 0;
}

int
access (const char *path, int mode)
{
  if (memfs_find (path) < 0 || (mode & W_OK))
    {
      errno = ENOENT;
      return -1;
    }
  return 0;
}

int
unlink (const char *path)
{
  (void) path;
  errno = EACCES;
  return -1;
}

int
dup (int fd)
{
  (void) fd;
  errno = EBADF;
  return -1;
}

int
fcntl (int fd, int cmd, ...)
{
  va_list ap;
  int arg = 0;
  va_start (ap, cmd);
  if (cmd == F_SETFL)
    arg = va_arg (ap, int);
  va_end (ap);
  if (!socket_fd (fd))
    {
      errno = EBADF;
      return -1;
    }
  if (cmd == F_GETFL)
    return O_RDWR;
  if (cmd == F_SETFL)
    {
      int r[10], on = (arg & O_NONBLOCK) != 0;
      memset (r, 0, sizeof r);
      r[0] = fd;
      r[1] = (int) FIONBIO;
      r[2] = (int) &on;
      if (rdps_swi (Socket_Ioctl, r))
        return set_errno_from (NULL);
      return 0;
    }
  errno = EINVAL;
  return -1;
}

/* --- sockets -------------------------------------------------------------- */

static int
sock_swi (int swi, int *r)
{
  _kernel_oserror *e = rdps_swi (swi, r);
  if (e)
    return set_errno_from (e);
  return r[0];
}

int
socket (int domain, int type, int protocol)
{
  int r[10], s;
  memset (r, 0, sizeof r);
  r[0] = domain;
  r[1] = type;
  r[2] = protocol;
  s = sock_swi (Socket_Creat, r);
  if (s >= 0)
    mark_socket (s, 1);
  return s;
}

int
bind (int fd, const struct sockaddr *a, socklen_t len)
{
  int r[10];
  memset (r, 0, sizeof r);
  r[0] = fd;
  r[1] = (int) a;
  r[2] = (int) len;
  return sock_swi (Socket_Bind, r) < 0 ? -1 : 0;
}

int
listen (int fd, int n)
{
  int r[10];
  memset (r, 0, sizeof r);
  r[0] = fd;
  r[1] = n;
  return sock_swi (Socket_Listen, r) < 0 ? -1 : 0;
}

int
accept (int fd, struct sockaddr *a, socklen_t *len)
{
  int r[10], s;
  memset (r, 0, sizeof r);
  r[0] = fd;
  r[1] = (int) a;
  r[2] = (int) len;
  s = sock_swi (Socket_Accept_1, r);
  if (s >= 0)
    mark_socket (s, 1);
  return s;
}

int
connect (int fd, const struct sockaddr *a, socklen_t len)
{
  int r[10];
  memset (r, 0, sizeof r);
  r[0] = fd;
  r[1] = (int) a;
  r[2] = (int) len;
  return sock_swi (Socket_Connect, r) < 0 ? -1 : 0;
}

ssize_t
recv (int fd, void *buf, size_t n, int flags)
{
  int r[10];
  memset (r, 0, sizeof r);
  r[0] = fd;
  r[1] = (int) buf;
  r[2] = (int) n;
  r[3] = flags;
  return sock_swi (Socket_Recv, r);
}

ssize_t
send (int fd, const void *buf, size_t n, int flags)
{
  int r[10];
  memset (r, 0, sizeof r);
  r[0] = fd;
  r[1] = (int) buf;
  r[2] = (int) n;
  r[3] = flags;
  return sock_swi (Socket_Send, r);
}

int
shutdown (int fd, int how)
{
  int r[10];
  memset (r, 0, sizeof r);
  r[0] = fd;
  r[1] = how;
  return sock_swi (Socket_Shutdown, r) < 0 ? -1 : 0;
}

int
setsockopt (int fd, int level, int name, const void *val, socklen_t len)
{
  int r[10];
  memset (r, 0, sizeof r);
  r[0] = fd;
  r[1] = level;
  r[2] = name;
  r[3] = (int) val;
  r[4] = (int) len;
  return sock_swi (Socket_Setsockopt, r) < 0 ? -1 : 0;
}

int
getsockopt (int fd, int level, int name, void *val, socklen_t *len)
{
  int r[10];
  memset (r, 0, sizeof r);
  r[0] = fd;
  r[1] = level;
  r[2] = name;
  r[3] = (int) val;
  r[4] = (int) len;
  return sock_swi (Socket_Getsockopt, r) < 0 ? -1 : 0;
}

int
getpeername (int fd, struct sockaddr *a, socklen_t *len)
{
  int r[10];
  memset (r, 0, sizeof r);
  r[0] = fd;
  r[1] = (int) a;
  r[2] = (int) len;
  return sock_swi (Socket_Getpeername_1, r) < 0 ? -1 : 0;
}

/* Never waits: the module must not sit in a SWI (a wait is the server
   thread's rdps_wait_cs). */
int
select (int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv)
{
  int r[10];
  struct timeval zero;
  (void) tv;
  zero.tv_sec = 0;
  zero.tv_usec = 0;
  memset (r, 0, sizeof r);
  r[0] = nfds;
  r[1] = (int) rd;
  r[2] = (int) wr;
  r[3] = (int) ex;
  r[4] = (int) &zero;
  return sock_swi (Socket_Select, r);
}

int
poll (struct pollfd *p, nfds_t n, int timeout)
{
  fd_set rd, wr;
  nfds_t i;
  int maxfd = -1, count = 0;
  (void) timeout;
  FD_ZERO (&rd);
  FD_ZERO (&wr);
  for (i = 0; i < n; i++)
    {
      p[i].revents = 0;
      if (!socket_fd (p[i].fd))
        continue;
      if (p[i].events & POLLIN)
        FD_SET (p[i].fd, &rd);
      if (p[i].events & POLLOUT)
        FD_SET (p[i].fd, &wr);
      if (p[i].fd > maxfd)
        maxfd = p[i].fd;
    }
  if (maxfd < 0)
    return 0;
  if (select (maxfd + 1, &rd, &wr, NULL, NULL) < 0)
    return -1;
  for (i = 0; i < n; i++)
    {
      if (!socket_fd (p[i].fd))
        continue;
      if ((p[i].events & POLLIN) && FD_ISSET (p[i].fd, &rd))
        p[i].revents |= POLLIN;
      if ((p[i].events & POLLOUT) && FD_ISSET (p[i].fd, &wr))
        p[i].revents |= POLLOUT;
      if (p[i].revents)
        count++;
    }
  return count;
}

in_addr_t
inet_aton (const char *s, struct in_addr *a)
{
  unsigned part[4];
  int n = 0;
  while (n < 4)
    {
      unsigned v = 0;
      int digits = 0;
      while (*s >= '0' && *s <= '9' && digits < 4)
        {
          v = v * 10 + (unsigned) (*s++ - '0');
          digits++;
        }
      if (!digits || v > 255)
        return 0;
      part[n++] = v;
      if (n < 4)
        {
          if (*s != '.')
            return 0;
          s++;
        }
    }
  if (*s)
    return 0;
  a->s_addr = htonl ((part[0] << 24) | (part[1] << 16) | (part[2] << 8) | part[3]);
  return 1;
}

char *
inet_ntoa (struct in_addr a)
{
  static char buf[16];
  unsigned v = ntohl (a.s_addr);
  rdps_snprintf_buf (buf, sizeof buf, "%u.%u.%u.%u", (v >> 24) & 255,
                     (v >> 16) & 255, (v >> 8) & 255, v & 255);
  return buf;
}

struct hostent *
gethostbyname (const char *name)
{
  (void) name;   /* numeric addresses only (Choices) */
  return NULL;
}

/* --- time ----------------------------------------------------------------- */

int
gettimeofday (struct timeval *__restrict tv, __timezone_ptr_t tz)
{
  unsigned char b[5];
  int r[10];
  unsigned long long cs;
  (void) tz;
  b[0] = 3;                         /* OS_Word 14,3: 5-byte UTC, cs since 1900 */
  memset (r, 0, sizeof r);
  r[0] = 14;
  r[1] = (int) b;
  if (rdps_swi (OS_Word, r))
    memset (b, 0, sizeof b);
  cs = (unsigned long long) b[0] | ((unsigned long long) b[1] << 8)
       | ((unsigned long long) b[2] << 16) | ((unsigned long long) b[3] << 24)
       | ((unsigned long long) b[4] << 32);
  tv->tv_sec = (time_t) (cs / 100 - 2208988800ULL);
  tv->tv_usec = (suseconds_t) ((cs % 100) * 10000);
  return 0;
}

struct tm *
localtime (const time_t *t)
{
  static struct tm tm;
  long days = (long) (*t / 86400), secs = (long) (*t % 86400);
  long z, era, doe, yoe, doy, mp, y;
  memset (&tm, 0, sizeof tm);
  if (secs < 0)
    {
      secs += 86400;
      days--;
    }
  tm.tm_hour = (int) (secs / 3600);
  tm.tm_min = (int) (secs / 60 % 60);
  tm.tm_sec = (int) (secs % 60);
  tm.tm_wday = (int) ((days + 4) % 7);
  if (tm.tm_wday < 0)
    tm.tm_wday += 7;
  /* days to civil (H. Hinnant's algorithm) */
  z = days + 719468;
  era = (z >= 0 ? z : z - 146096) / 146097;
  doe = z - era * 146097;
  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = yoe + era * 400;
  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  mp = (5 * doy + 2) / 153;
  tm.tm_mday = (int) (doy - (153 * mp + 2) / 5 + 1);
  tm.tm_mon = (int) (mp < 10 ? mp + 2 : mp - 10);
  if (tm.tm_mon <= 1)
    y++;
  tm.tm_year = (int) (y - 1900);
  tm.tm_zone = "UTC";
  return &tm;
}

size_t
strftime (char *s, size_t size, const char *fmt, const struct tm *tm)
{
  size_t n = 0;
  char tmp[32];
  if (size == 0)
    return 0;
  for (; *fmt; fmt++)
    {
      const char *add = tmp;
      if (*fmt != '%')
        {
          tmp[0] = *fmt;
          tmp[1] = 0;
        }
      else
        switch (*++fmt)
          {
          case 'Y': rdps_snprintf_buf (tmp, sizeof tmp, "%d", tm->tm_year + 1900); break;
          case 'm': rdps_snprintf_buf (tmp, sizeof tmp, "%02d", tm->tm_mon + 1); break;
          case 'd': rdps_snprintf_buf (tmp, sizeof tmp, "%02d", tm->tm_mday); break;
          case 'H': rdps_snprintf_buf (tmp, sizeof tmp, "%02d", tm->tm_hour); break;
          case 'M': rdps_snprintf_buf (tmp, sizeof tmp, "%02d", tm->tm_min); break;
          case 'S': rdps_snprintf_buf (tmp, sizeof tmp, "%02d", tm->tm_sec); break;
          case 'F':
            rdps_snprintf_buf (tmp, sizeof tmp, "%d-%02d-%02d", tm->tm_year + 1900,
                               tm->tm_mon + 1, tm->tm_mday);
            break;
          case 'T':
            rdps_snprintf_buf (tmp, sizeof tmp, "%02d:%02d:%02d", tm->tm_hour,
                               tm->tm_min, tm->tm_sec);
            break;
          case 'z': add = "+0000"; break;
          case 'Z': add = "UTC"; break;
          case '%': add = "%"; break;
          case 0: fmt--; add = ""; break;
          default: tmp[0] = '%'; tmp[1] = *fmt; tmp[2] = 0; break;
          }
      while (*add)
        {
          if (n + 1 >= size)
            {
              s[n] = 0;
              return 0;
            }
          s[n++] = *add++;
        }
    }
  s[n] = 0;
  return n;
}

int
usleep (useconds_t us)
{
  rdps_wait_cs ((unsigned) ((us + 9999) / 10000));
  return 0;
}

/* --- process -------------------------------------------------------------- */

pid_t
getpid (void)
{
  return 1;
}

char *
getenv (const char *name)
{
  (void) name;
  return NULL;
}

__sighandler_t
signal (int sig, __sighandler_t h)
{
  (void) sig;
  (void) h;
  return SIG_DFL;
}

int
raise (int sig)
{
  (void) sig;
  return 0;
}

int
atexit (void (*f) (void))
{
  (void) f;
  return 0;
}

void
exit (int status)
{
  rdps_fatal (status == 0 ? "the server stopped" : "the server stopped after an error");
  for (;;)
    ;   /* only from outside the server thread, which never calls exit */
}

void
abort (void)
{
  rdps_fatal ("the server stopped (abort)");
  for (;;)
    ;
}

void
openlog (const char *ident, int opt, int facility)
{
  (void) ident; (void) opt; (void) facility;
}

void
closelog (void)
{
}

void
syslog (int pri, const char *fmt, ...)
{
  char buf[200];
  va_list ap;
  (void) pri;
  va_start (ap, fmt);
  rdps_vformat (buf, sizeof buf, fmt, ap);
  va_end (ap);
  rdps_console_line ('x', buf);
}
