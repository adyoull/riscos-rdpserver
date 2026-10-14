/* RDPServer module: the server as a relocatable module.

   Why a module: a Wimp task gets no time while another program holds the
   desktop, and none at all while a Wimp error box is open (the Wimp stops
   multitasking until it's answered: Wimp source s/Wimp07, "pollit"). A
   module's code is always there, so the server can run whatever task is
   paged in:

   - normally from a transient callback, added every 2 cs by an
     OS_CallEvery ticker (the Wimp lets callbacks run when it's idle:
     s/Wimp03 "triggercallbacks"; and they run whenever a program returns
     to USR mode);
   - while an error box is open (Service_WimpReportError &57, R0 1 opening,
     0 closing: s/Wimp07), from ByteV: the error box's loop calls OS_Byte
     &81 with R1 = R2 = 0 on every pass (pollit), in SVC mode, where
     callbacks never run.

   Either way the server runs in its thread (srvthread.h, coro.h): on its
   own stack, until it next waits. The front end (the !RDPServer task)
   uses the entries in modapi.h.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>

#include "modapi.h"
#include "modrt.h"
#include "../platform.h"
#include "../server.h"
#include "../srvthread.h"

/* header.s */
extern void mod_ticker_veneer (void);
extern void mod_callback_veneer (void);
extern void mod_bytev_veneer (void);
extern volatile int mod_errorbox_open;   /* read by the ByteV veneer */

#define ByteV 6
#define STACK_BYTES (512 * 1024)
#define HEAP_MAX (48 * 1024 * 1024)
#define TICKER_CS 1                       /* OS_CallEvery: every 2 cs */
#define EXTRA_TURNS 0                     /* see drive(); RDPServer$ExtraTurns */

static struct
{
  int *private_word;
  int ticker, bytev;                      /* installed */
  volatile int callback_pending;
  volatile int driving;                   /* running the thread: no nesting */
  uintptr_t driving_sp;                   /* the SVC sp where that began */
  unsigned driving_since;                 /* and when (cs) */
  int started;                            /* mod_start succeeded, no stop since */
  int restart;                            /* start again (the thread was lost) */
  unsigned lost;
  int eager;                              /* the next callback runs the thread, due or not */
  int extra;                              /* extra turns in this burst */
  unsigned extra_cs;                      /* when the burst began */
  int extra_max;                          /* EXTRA_TURNS, or RDPServer$ExtraTurns */
  int attached_task;                      /* the front end, or 0 */
  int dead;
  void *stack;
  unsigned callbacks, errorbox_steps;
  server_options opt;                     /* what the server runs with */
  rdps_mod_options mopt;
} m;

volatile int mod_errorbox_open;

/* --- messages for the front end ------------------------------------------- */

#define RING 96
#define LINE 200
static struct
{
  unsigned seq;                           /* number of the newest line */
  char line[RING][LINE];                  /* "<tag> <text>" */
} ring;

void
rdps_console_line (char tag, const char *text)
{
  char *l;
  size_t n;
  ring.seq++;
  l = ring.line[ring.seq % RING];
  l[0] = tag;
  l[1] = ' ';
  n = strlen (text);
  if (n > LINE - 3)
    n = LINE - 3;
  memcpy (l + 2, text, n);
  l[2 + n] = 0;
}

static void
say_hook (const char *line)
{
  rdps_console_line ('s', line);
}

int
mod_messages (unsigned *seq, char *buf, int size)
{
  int used = 0;
  unsigned s = *seq;
  if (size <= 0)
    return 0;
  if (ring.seq - s > RING)
    {
      used = rdps_snprintf_buf (buf, (size_t) size, "s (%u lines missed)\n",
                                ring.seq - s - RING);
      if (used >= size)
        return 0;
      s = ring.seq - RING;
    }
  while (s != ring.seq)
    {
      const char *l = ring.line[(s + 1) % RING];
      size_t n = strlen (l);
      if ((int) (used + n + 2) > size)
        break;
      memcpy (buf + used, l, n);
      used += (int) n;
      buf[used++] = '\n';
      s++;
    }
  buf[used < size ? used : size - 1] = 0;
  *seq = s;
  return used;
}

/* --- running the server --------------------------------------------------- */

/* The thread can be run from here (SVC mode): not already running it.
   One that is shows as m.driving set with its frames on the SVC stack
   below driving_sp, so anything that runs meanwhile (an interrupt, a SWI
   it makes) has sp below that. If m.driving is set but sp isn't below
   it, the run that set it has gone: its frames were thrown away (a
   program was stopped while the thread ran in its time, and the stop
   unwound the SVC stack too) and the thread will never yield back to
   it. A transient callback that stops the current task (Alt-Break on a
   runaway program) can come inside the thread, which runs in USR mode:
   then the server starts again. */
static uintptr_t
svc_sp (void)
{
  uintptr_t sp;
  __asm__ volatile ("mov %0, sp" : "=r" (sp));
  return sp;
}

static void
lost (void)
{
  /* Anything nested in the clean-up below sees a run in progress here,
     not another loss. */
  m.driving = 1;
  m.driving_sp = svc_sp ();
  m.driving_since = plat_time_cs ();
  m.eager = 0;
  m.lost++;
  rdps_console_line ('s', "the server was lost (a program was stopped in the "
                     "middle of the server's turn): starting it again");
  {
    /* The client's connection is cut, not closed: a reset, with no
       disconnect, is a broken connection to the client, which may then
       connect again by itself with its auto-reconnect cookie. */
    extern int rdps_socket_is_ours (int fd);
    int sck = server_client_socket ();
    if (sck >= 0 && rdps_socket_is_ours (sck))   /* not another program's by now */
      {
        struct linger l;
        l.l_onoff = 1;
        l.l_linger = 0;
        setsockopt (sck, SOL_SOCKET, SO_LINGER, &l, sizeof l);
      }
  }
  rdps_thread_reset ();
  rdps_close_all_sockets ();
  plat_click_finish ();
  m.driving = 0;
  if (rdps_thread_init (m.stack, STACK_BYTES) != 0)
    {
      m.dead = 1;
      return;
    }
  m.restart = m.started;
}

static int
take (void)
{
  uintptr_t sp = svc_sp ();
  if (m.dead)
    return 0;
  if (m.driving)
    {
      /* Inside a run (deeper on the SVC stack): no nesting. A run yields
         every few cs (the RSA step, the longest, takes 16 cs on a Pi 4),
         so one "in progress" for 5 s from a deeper caller (a ByteV in an
         error box opened from deep code, say) is lost too. */
      if (sp < m.driving_sp
          && (int) (plat_time_cs () - m.driving_since) < 500)
        return 0;
      lost ();
      if (m.dead)
        return 0;
    }
  m.driving = 1;
  m.driving_sp = sp;
  m.driving_since = plat_time_cs ();
  return 1;
}

static void
give (void)
{
  m.driving = 0;
}

static void add_callback (void);

static void
drive (void)
{
  int eager = m.eager;
  m.eager = 0;
  if (!take ())
    return;
  if (m.restart)
    {
      m.restart = 0;
      if (rdps_cmd_start (&m.opt) != 0)
        m.started = 0;
    }
  {
    unsigned sent = rdps_bytes_sent, now;
    if (eager || rdps_thread_due (plat_time_cs ()))
      rdps_thread_run ();
    give ();
    /* Sent something and has more to do soon (it's waiting for room to
       send, or for its next band of the screen): ask for the next turn
       straight away, not at the next tick (2 cs). Callbacks already
       queued run first - the Internet module's, which take in the
       client's ACKs and send what's queued - so the turns and the
       Internet module's work interleave, as they did inside the turn
       when the thread ran in USR mode. Only after progress, and in
       bursts of at most EXTRA_TURNS turns within 1 cs, at most one burst
       per 2 cs: the desktop keeps at least half its time. (A count
       reset by the ticker wouldn't do: a turn can outlast a tick.) */
    now = plat_time_cs ();
    if ((int) (now - m.extra_cs) >= 2)
      {
        m.extra_cs = now;
        m.extra = 0;
      }
    if (rdps_bytes_sent != sent && !m.dead && m.extra < m.extra_max
        && (int) (now - m.extra_cs) < 1
        && (int) (rdps_thread_wake_time () - now) <= 1)
      {
        m.extra++;
        m.eager = 1;
        add_callback ();
      }
  }
}

/* Asks for the callback, if one isn't on its way. From the ticker
   (interrupts off) and from drive() (interrupts on): the check and the
   request are made with interrupts off, so the ticker can't slip in
   between and ask too (two on the chain, and finalisation would remove
   only one). */
static void
add_callback (void)
{
  int r[10];
  unsigned psr;
  __asm__ volatile ("mrs %0, cpsr\n\torr r0, %0, #0x80\n\tmsr cpsr_c, r0"
                    : "=r" (psr) : : "r0", "memory");
  if (!m.callback_pending && !m.dead)
    {
      memset (r, 0, sizeof r);
      r[0] = (int) mod_callback_veneer;
      r[1] = (int) m.private_word;
      if (rdps_swi (OS_AddCallBack, r) == NULL)
        m.callback_pending = 1;
    }
  __asm__ volatile ("msr cpsr_c, %0" : : "r" (psr) : "memory");
}

/* The ticker (SVC mode, interrupts off): ask for a callback if one isn't
   already on its way. */
void
mod_ticker (void)
{
  add_callback ();
}

/* The callback (SVC mode, interrupts on). */
void
mod_callback (void)
{
  m.callback_pending = 0;
  if (!m.driving && rdps_thread_due (plat_time_cs ()))
    m.callbacks++;
  drive ();
}

/* ByteV, OS_Byte &81 with R1 = R2 = 0 while an error box is open (SVC
   mode, interrupts turned on by the veneer). */
void
mod_bytev (void)
{
  if (!mod_errorbox_open)
    return;
  if (rdps_thread_due (plat_time_cs ()))
    m.errorbox_steps++;
  drive ();
}

void
mod_service (int r0, int service, int r2)
{
  if (service == 0x57)          /* Service_WimpReportError: 1 opening, 0 closing */
    mod_errorbox_open = r0 != 0;
  else if (service == 0x53 && r0 == 0 && r2 != 0 && r2 == m.attached_task)
    {
      /* Service_WimpCloseDown for the front end: it has gone (perhaps
         without detaching), so stop serving. */
      if (take ())
        {
          rdps_cmd_stop ();
          m.started = 0;
          m.restart = 0;
          give ();
        }
      m.attached_task = 0;
    }
}

void
rdps_fatal (const char *why)
{
  rdps_console_line ('s', why);
  m.dead = 1;
  rdps_close_all_sockets ();
  plat_click_finish ();
  plat_set_clients (0);
  for (;;)
    {
      extern void co_yield (void);
      extern int co_inside (void);
      if (!co_inside ())
        {
          static const struct { int n; char s[40]; } e =
            { 0x1e6, "RDPServer: the server stopped" };
          int r[10];
          memset (r, 0, sizeof r);
          r[0] = (int) &e;
          rdps_swi (OS_GenerateError, r);
          return;
        }
      co_yield ();   /* never resumed again: dead */
    }
}

/* --- the front end's entries (modapi.h; SVC mode) ------------------------- */

int
mod_attach (int task)
{
  if (m.attached_task && m.attached_task != task)
    {
      /* Still there? (TaskManager_TaskNameFromHandle fails if not.) */
      int r[10];
      memset (r, 0, sizeof r);
      r[0] = m.attached_task;
      if (rdps_swi (TaskManager_TaskNameFromHandle, r) == NULL)
        return 1;
    }
  m.attached_task = task;
  return 0;
}

void
mod_stop (void)
{
  if (!take ())
    return;
  rdps_cmd_stop ();
  m.started = 0;
  m.restart = 0;
  give ();
}

void
mod_detach (void)
{
  mod_stop ();
  m.attached_task = 0;
}

int
mod_set_file (const char *path, const char *data, int len)
{
  if (!path || len < 0)
    return -1;
  return rdps_memfs_set (path, data, (size_t) len);
}

int
mod_start (const rdps_mod_options *o)
{
  int r;
  if (!take ())
    return 1;
  m.started = 0;
  m.restart = 0;
  m.mopt = *o;
  m.mopt.port[sizeof m.mopt.port - 1] = 0;
  m.mopt.address[sizeof m.mopt.address - 1] = 0;
  m.mopt.ini[sizeof m.mopt.ini - 1] = 0;
  m.mopt.logins[sizeof m.mopt.logins - 1] = 0;
  memset (&m.opt, 0, sizeof m.opt);
  m.opt.port = m.mopt.port;
  m.opt.address = m.mopt.address;
  m.opt.ini = m.mopt.ini;
  m.opt.logins = m.mopt.logins;
  m.opt.interval_cs = m.mopt.interval_cs ? m.mopt.interval_cs : 10;
  m.opt.idle_interval_cs = m.mopt.idle_interval_cs;
  m.opt.handshake_cs = m.mopt.handshake_cs;
  m.opt.rows_per_scan = m.mopt.rows_per_scan;
  m.opt.verbose = m.mopt.verbose;
  m.opt.log_file = NULL;               /* libxrdp's log: to the messages */
  {
    /* RDPServer$ThreadMode SYS: the thread in SYS mode, for comparing
       (0.2-test6 to test8's: no callbacks in its turn, so the Internet
       module's work waits and sending is slow). */
    extern int co_thread_mode;
    char v[8];
    int len = -1;
    co_thread_mode = 0x10;
    if (_swix (OS_ReadVarVal, _INR (0, 4) | _OUT (2), "RDPServer$ThreadMode", v,
               (int) sizeof v - 1, 0, 3, &len) == NULL && len == 3
        && (v[0] | 32) == 's' && (v[1] | 32) == 'y' && (v[2] | 32) == 's')
      {
        co_thread_mode = 0x1F;
        rdps_console_line ('s', "the server thread runs in SYS mode "
                           "(RDPServer$ThreadMode): for comparing only");
      }
  }

  {
    /* RDPServer$ExtraTurns 0..8: see drive(). Off by default: with the
       thread in USR mode the Internet module's work runs inside the turn. */
    char v[8];
    int len = -1;
    m.extra_max = EXTRA_TURNS;
    if (_swix (OS_ReadVarVal, _INR (0, 4) | _OUT (2), "RDPServer$ExtraTurns", v,
               (int) sizeof v - 1, 0, 3, &len) == NULL && len > 0 && len < (int) sizeof v)
      {
        v[len] = 0;
        if (v[0] >= '0' && v[0] <= '8' && v[1] == 0)
          m.extra_max = v[0] - '0';
      }
    if (m.extra_max != EXTRA_TURNS)
      {
        char line[80];
        rdps_snprintf_buf (line, sizeof line, "extra turns: %d (RDPServer$ExtraTurns)",
                           m.extra_max);
        rdps_console_line ('s', line);
      }
  }
  r = rdps_cmd_start (&m.opt);
  m.started = r == 0;
  give ();
  return r;
}

void
mod_disconnect (void)
{
  if (!take ())
    return;
  rdps_cmd_disconnect ();
  give ();
}

int
mod_reload_logins (void)
{
  int r;
  if (!take ())
    return -1;
  r = rdps_cmd_reload_logins ();
  give ();
  return r;
}

int
mod_status (rdps_mod_status *s)
{
  memset (s, 0, sizeof *s);
  server_get_status (&s->st);
  s->st.listening = s->st.listening && rdps_thread_running ();
  s->errorbox_open = mod_errorbox_open;
  s->callbacks = m.callbacks;
  s->errorbox_steps = m.errorbox_steps;
  s->heap_bytes = (unsigned) rdps_heap_used ();
  s->dead = m.dead;
  return 0;
}

/* --- initialisation and finalisation -------------------------------------- */

static const struct { int n; char s[64]; } err_nomem =
  { 0x1e6, "RDPServer: not enough memory for its dynamic area" };
static const struct { int n; char s[64]; } err_inuse =
  { 0x1e6, "RDPServer is in use: quit !RDPServer first" };
static const struct { int n; char s[64]; } err_tidy =
  { 0x1e6, "RDPServer can't be moved while it runs" };

static void
remove_handlers (void)
{
  int r[10];
  if (m.ticker)
    {
      memset (r, 0, sizeof r);
      r[0] = (int) mod_ticker_veneer;
      r[1] = (int) m.private_word;
      rdps_swi (OS_RemoveTickerEvent, r);
      m.ticker = 0;
    }
  memset (r, 0, sizeof r);
  r[0] = (int) mod_callback_veneer;
  r[1] = (int) m.private_word;
  rdps_swi (OS_RemoveCallBack, r);
  m.callback_pending = 0;
  if (m.bytev)
    {
      memset (r, 0, sizeof r);
      r[0] = ByteV;
      r[1] = (int) mod_bytev_veneer;
      r[2] = (int) m.private_word;
      rdps_swi (OS_Release, r);
      m.bytev = 0;
    }
}

/* r12 on entry: the private word. Returns an error or NULL. */
const void *
mod_init (int *private_word)
{
  int r[10];
  memset (&m, 0, sizeof m);
  memset (&ring, 0, sizeof ring);
  mod_errorbox_open = 0;
  m.private_word = private_word;
  *private_word = (int) &m;               /* OS_Module 18 gives this as R4 */
  rdps_ctype_init ();
  if (rdps_heap_init ("RDPServer", HEAP_MAX) != 0)
    return &err_nomem;
  m.stack = malloc (STACK_BYTES);
  if (!m.stack || rdps_thread_init (m.stack, STACK_BYTES) != 0)
    {
      rdps_heap_final ();
      return &err_nomem;
    }
  server_set_message_hook (say_hook);

  memset (r, 0, sizeof r);
  r[0] = ByteV;
  r[1] = (int) mod_bytev_veneer;
  r[2] = (int) private_word;
  if (rdps_swi (OS_Claim, r) == NULL)
    m.bytev = 1;
  memset (r, 0, sizeof r);
  r[0] = TICKER_CS;
  r[1] = (int) mod_ticker_veneer;
  r[2] = (int) private_word;
  if (rdps_swi (OS_CallEvery, r) == NULL)
    m.ticker = 1;
  if (!m.ticker || !m.bytev)
    {
      remove_handlers ();
      rdps_heap_final ();
      return &err_nomem;
    }
  rdps_console_line ('s', "module started");
  return NULL;
}

/* fatal: R10 on entry (0 = RMTidy). */
const void *
mod_final (int fatal)
{
  if (!fatal)
    return &err_tidy;
  if (m.attached_task)
    {
      int r[10];
      memset (r, 0, sizeof r);
      r[0] = m.attached_task;
      if (rdps_swi (TaskManager_TaskNameFromHandle, r) == NULL)
        return &err_inuse;
    }
  if (take ())
    {
      rdps_cmd_stop ();
      m.started = 0;
      m.restart = 0;
      give ();
    }
  remove_handlers ();
  plat_click_finish ();
  rdps_close_all_sockets ();
  plat_set_clients (0);
  rdps_memfs_clear ();
  rdps_heap_final ();
  if (m.private_word)
    *m.private_word = 0;
  return NULL;
}
