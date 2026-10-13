/* RDPServer: the parts of xrdp's os_calls interface (common/os_calls.h)
   that libxrdp and the common files it uses need, written for UnixLib
   (and any POSIX system, for the Linux test build). IPv4 only; no Unix
   domain or VSOCK sockets, no processes, no shared memory.

   What each call returns follows xrdp 0.10.6's os_calls.c.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "arch.h"
#include "os_calls.h"
#include "thread_calls.h"

/* --- text output ---------------------------------------------------------- */

void
g_printf (const char *format, ...)
{
  va_list ap;
  va_start (ap, format);
  vfprintf (stdout, format, ap);
  va_end (ap);
}

void
g_sprintf (char *dest, const char *format, ...)
{
  va_list ap;
  va_start (ap, format);
  vsprintf (dest, format, ap);
  va_end (ap);
}

int
g_snprintf (char *dest, int len, const char *format, ...)
{
  int n;
  va_list ap;
  va_start (ap, format);
  n = vsnprintf (dest, (size_t) len, format, ap);
  va_end (ap);
  return n;
}

void
g_writeln (const char *format, ...)
{
  va_list ap;
  va_start (ap, format);
  vfprintf (stdout, format, ap);
  va_end (ap);
  fputc ('\n', stdout);
}

char *
g_get_strerror (void)
{
  return strerror (errno);
}

/* --- sockets -------------------------------------------------------------- */

int
g_tcp_socket (void)
{
  int s = socket (AF_INET, SOCK_STREAM, 0);
  if (s >= 0)
    {
      int one = 1;
      setsockopt (s, IPPROTO_TCP, TCP_NODELAY, (char *) &one, sizeof one);
    }
  return s;
}

int
g_tcp4_socket (void)
{
  return g_tcp_socket ();
}

int
g_tcp6_socket (void)
{
  errno = EAFNOSUPPORT;
  return -1;
}

int
g_sck_local_socket (void)
{
  errno = EAFNOSUPPORT;
  return -1;
}

int
g_sck_vsock_socket (void)
{
  errno = EAFNOSUPPORT;
  return -1;
}

int
g_sck_set_reuseaddr (int sck)
{
  int one = 1;
  return setsockopt (sck, SOL_SOCKET, SO_REUSEADDR, (char *) &one, sizeof one);
}

void
g_sck_close (int sck)
{
  if (sck >= 0)
    close (sck);
}

int
g_sck_set_non_blocking (int sck)
{
  int f = fcntl (sck, F_GETFL, 0);
  if (f < 0)
    return -1;
  return fcntl (sck, F_SETFL, f | O_NONBLOCK) < 0 ? -1 : 0;
}

static int
fill_addr (struct sockaddr_in *sa, const char *port, const char *address)
{
  memset (sa, 0, sizeof *sa);
  sa->sin_family = AF_INET;
  sa->sin_port = htons ((unsigned short) atoi (port));
  if (address == NULL || address[0] == 0 || strcmp (address, "0.0.0.0") == 0)
    sa->sin_addr.s_addr = htonl (INADDR_ANY);
  else if (strcmp (address, "localhost") == 0)
    sa->sin_addr.s_addr = htonl (INADDR_LOOPBACK);
  else if (inet_aton (address, &sa->sin_addr) == 0)
    {
      struct hostent *h = gethostbyname (address);
      if (h == NULL || h->h_addrtype != AF_INET)
        return -1;
      memcpy (&sa->sin_addr, h->h_addr_list[0], 4);
    }
  return 0;
}

int
g_tcp_bind_address (int sck, const char *port, const char *address)
{
  struct sockaddr_in sa;
  if (fill_addr (&sa, port, address) != 0)
    return -1;
  return bind (sck, (struct sockaddr *) &sa, sizeof sa);
}

int
g_tcp4_bind_address (int sck, const char *port, const char *address)
{
  return g_tcp_bind_address (sck, port, address);
}

int
g_tcp6_bind_address (int sck, const char *port, const char *address)
{
  (void) sck; (void) port; (void) address;
  return -1;
}

int
g_sck_local_bind (int sck, const char *port)
{
  (void) sck; (void) port;
  return -1;
}

int
g_sck_vsock_bind_address (int sck, const char *port, const char *address)
{
  (void) sck; (void) port; (void) address;
  return -1;
}

int
g_tcp_connect (int sck, const char *address, const char *port)
{
  struct sockaddr_in sa;
  if (fill_addr (&sa, port, address) != 0)
    return -1;
  return connect (sck, (struct sockaddr *) &sa, sizeof sa);
}

int
g_sck_local_connect (int sck, const char *port)
{
  (void) sck; (void) port;
  return -1;
}

int
g_sck_listen (int sck)
{
  return listen (sck, 2);
}

int
g_sck_accept (int sck)
{
  struct sockaddr_in sa;
  socklen_t len = sizeof sa;
  return accept (sck, (struct sockaddr *) &sa, &len);
}

int
g_sck_recv (int sck, void *ptr, unsigned int len, int flags)
{
  return (int) recv (sck, ptr, len, flags);
}

int
g_sck_send (int sck, const void *ptr, unsigned int len, int flags)
{
  return (int) send (sck, ptr, len, flags);
}

int
g_sck_last_error_would_block (int sck)
{
  (void) sck;
  return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS;
}

int
g_sck_socket_ok (int sck)
{
  int opt = 0;
  socklen_t len = sizeof opt;
  return getsockopt (sck, SOL_SOCKET, SO_ERROR, (char *) &opt, &len) == 0
         && opt == 0;
}

static int
poll_one (int sck, short events, int millis)
{
  struct pollfd p;
  if (sck < 0)
    return 0;
  p.fd = sck;
  p.events = events;
  p.revents = 0;
  return poll (&p, 1, millis) > 0 && (p.revents & (events | POLLHUP)) != 0;
}

int
g_sck_can_send (int sck, int millis)
{
  return poll_one (sck, POLLOUT, millis);
}

int
g_sck_can_recv (int sck, int millis)
{
  return poll_one (sck, POLLIN, millis);
}

const char *
g_sck_get_peer_ip_address (int sck, char *ip, unsigned int bytes,
                           unsigned short *port)
{
  struct sockaddr_in sa;
  socklen_t len = sizeof sa;
  if (bytes == 0)
    return ip;
  ip[0] = 0;
  if (port)
    *port = 0;
  if (getpeername (sck, (struct sockaddr *) &sa, &len) == 0
      && sa.sin_family == AF_INET)
    {
      snprintf (ip, bytes, "%s", inet_ntoa (sa.sin_addr));
      if (port)
        *port = ntohs (sa.sin_port);
    }
  return ip;
}

const char *
g_sck_get_peer_description (int sck, char *desc, unsigned int bytes)
{
  char ip[32];
  unsigned short port;
  g_sck_get_peer_ip_address (sck, ip, sizeof ip, &port);
  if (bytes > 0)
    snprintf (desc, bytes, "%s:%u", ip[0] ? ip : "?", (unsigned) port);
  return desc;
}

/* --- time ----------------------------------------------------------------- */

void
g_sleep (int msecs)
{
  usleep ((useconds_t) msecs * 1000);
}

int
g_time3 (void)
{
  struct timeval tp;
  gettimeofday (&tp, 0);
  return (int) ((tp.tv_sec * 1000) + (tp.tv_usec / 1000));
}

/* --- misc ----------------------------------------------------------------- */

/* Random bytes from /dev/urandom: on RISC OS, UnixLib serves it from the
   CryptRandom module. If that fails there is no safe fallback for keys
   and session randoms, so the program stops. */
void
g_random (char *data, int len)
{
  int fd = open ("/dev/urandom", O_RDONLY);
  int got = 0;
  if (fd >= 0)
    {
      while (got < len)
        {
          int n = (int) read (fd, data + got, (size_t) (len - got));
          if (n <= 0)
            break;
          got += n;
        }
      close (fd);
    }
  if (got != len)
    {
      fprintf (stderr, "RDPServer: no random numbers (/dev/urandom; on RISC OS "
               "the CryptRandom module)\n");
      exit (EXIT_FAILURE);
    }
}

int
g_abs (int i)
{
  return i < 0 ? -i : i;
}

int
g_memcmp (const void *s1, const void *s2, int len)
{
  return memcmp (s1, s2, (size_t) len);
}

char *
g_getenv (const char *name)
{
  return getenv (name);
}

int
g_getpid (void)
{
  return (int) getpid ();
}

/* --- files ---------------------------------------------------------------- */

int
g_file_open_ro (const char *file_name)
{
  return open (file_name, O_RDONLY);
}

int
g_file_close (int fd)
{
  return close (fd);
}

int
g_file_read (int fd, char *ptr, int len)
{
  return (int) read (fd, ptr, (size_t) len);
}

int
g_file_write (int fd, const char *ptr, int len)
{
  return (int) write (fd, ptr, (size_t) len);
}

int
g_file_seek (int fd, int offset)
{
  return (int) lseek (fd, offset, SEEK_SET);
}

int
g_file_set_cloexec (int fd, int status)
{
  (void) fd; (void) status;
  return 0;
}

int
g_chmod_hex (const char *filename, int flags)
{
  (void) filename; (void) flags;
  return 0;
}

int
g_file_readable (const char *filename)
{
  return access (filename, R_OK) == 0;
}

int
g_create_path (const char *path)
{
  (void) path;
  return 1;
}

int
g_file_delete (const char *filename)
{
  return unlink (filename) == 0;
}

int
g_file_get_size (const char *filename)
{
  struct stat st;
  if (stat (filename, &st) == 0)
    return (int) st.st_size;
  return -1;
}

/* --- threads: there's only one --------------------------------------------- */

tbus
tc_get_threadid (void)
{
  return 0;
}
