/* RDPServer: the server thread (see srvthread.h and coro.h).
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>

#include "coro.h"
#include "platform.h"
#include "server.h"
#include "srvthread.h"

enum { CMD_NONE, CMD_START, CMD_STOP };

static volatile int cmd;            /* waiting for the thread's loop */
static int cmd_result;
static const server_options *cmd_opt;
static volatile int dropping;       /* rdps_cmd_disconnect: end the session */
static volatile int stopping;       /* rdps_cmd_stop: end everything */
static int running;                 /* server_start succeeded */
static unsigned wake;               /* when the thread next wants to run */
static int ready;                   /* rdps_thread_init done */
unsigned rdps_yielded_cs;

static void
body (void *arg)
{
  (void) arg;
  for (;;)
    {
      unsigned w = 25;
      if (dropping)
        {
          server_disconnect ();
          dropping = 0;
        }
      switch (cmd)
        {
        case CMD_START:
          if (running)
            server_finish ();
          cmd_result = server_start (cmd_opt);
          running = cmd_result == 0;
          cmd = CMD_NONE;
          break;
        case CMD_STOP:
          if (running)
            server_finish ();
          running = 0;
          stopping = 0;
          cmd = CMD_NONE;
          break;
        default:
          break;
        }
      if (running)
        w = (unsigned) server_step (0);
      rdps_wait_cs (w);
    }
}

int
rdps_thread_init (void *stack, size_t size)
{
  if (ready)
    return 0;
  if (size < 65536 || co_start (body, NULL, stack, size) != 0)
    return -1;
  ready = 1;
  wake = plat_time_cs ();
  return 0;
}

/* The module's safety net: the thread was lost (a resume never came
   back). Forget it and its server; the caller closes the sockets and
   calls rdps_thread_init again. The big buffers are freed; what may be
   half changed (libxrdp's session) is left: a few KB each time. */
void
rdps_thread_reset (void)
{
  co_reset ();
  server_abandon ();
  cmd = CMD_NONE;
  dropping = 0;
  stopping = 0;
  running = 0;
  ready = 0;
}

int
rdps_thread_due (unsigned now)
{
  return ready && (cmd != CMD_NONE || dropping || (int) (now - wake) >= 0);
}

unsigned
rdps_thread_wake_time (void)
{
  return wake;
}

void
rdps_thread_run (void)
{
  if (ready && !co_inside ())
    co_resume ();
}

/* A restart ends whatever the thread is waiting for, as a stop does. */
int
rdps_thread_ending (void)
{
  return dropping || stopping || cmd == CMD_START;
}

int
rdps_thread_running (void)
{
  return running;
}

void
rdps_wait_cs (unsigned cs)
{
  if (co_inside ())
    {
      unsigned t = plat_time_cs ();
      wake = t + cs;
      co_yield ();
      rdps_yielded_cs += plat_time_cs () - t;
      return;
    }
#ifndef RDPS_MODULE
  if (cs)
    usleep (cs * 10000);
#endif
}

/* Resumes the thread until the command has been taken (it normally is at
   the first resume: the thread waits at the top of its loop; if it's in
   the middle of a wait, such as a client's connection sequence, that
   ends on the next few resumes). */
static void
run_until_done (void)
{
  int i;
  for (i = 0; i < 1000 && (cmd != CMD_NONE || dropping); i++)
    rdps_thread_run ();
}

int
rdps_cmd_start (const server_options *o)
{
  if (!ready)
    return 1;
  cmd_opt = o;
  cmd_result = 1;
  cmd = CMD_START;
  run_until_done ();
  return cmd == CMD_NONE ? cmd_result : 1;
}

void
rdps_cmd_stop (void)
{
  if (!ready)
    return;
  stopping = 1;
  cmd = CMD_STOP;
  run_until_done ();
  stopping = 0;
}

/* Done here, not in the thread: if a client is in the middle of
   connecting, the thread is waiting inside libxrdp and wouldn't get to a
   command before that client's login is checked against the old logins.
   The thread only ever stops at a wait, never in the middle of using
   them, so they can be replaced while it's stopped. */
int
rdps_cmd_reload_logins (void)
{
  if (!ready || co_inside ())
    return -1;
  return server_reload_logins ();
}

/* Likewise: only flags the thread reads at a wait. */
int
rdps_cmd_set_mute (int on)
{
  if (!ready || co_inside ())
    return -1;
  server_set_mute (on);
  return 0;
}

void
rdps_cmd_shutdown_complete (void)
{
  if (ready)
    server_shutdown_complete ();     /* (a flag) */
}

void
rdps_cmd_restarting (void)
{
  if (ready && !co_inside ())
    server_restarting ();
}

int
rdps_cmd_restart_done (void)
{
  return !ready || co_inside () || server_restart_done ();
}

int
rdps_cmd_set_verbose (int on)
{
  if (!ready || co_inside ())
    return -1;
  server_set_verbose (on);
  return 0;
}

int
rdps_cmd_set_sound (int on)
{
  if (!ready || co_inside ())
    return -1;
  server_set_sound (on);
  return 0;
}

int
rdps_cmd_set_require_tls (int on)
{
  if (!ready || co_inside ())
    return -1;
  server_set_require_tls (on);
  return 0;
}

void
rdps_cmd_disconnect (void)
{
  int sck;
  if (!ready)
    return;
  /* Shut the socket down here, not close it: the thread still owns the
     number and closes it when it ends the session (closed here, the
     number could be given to another program's socket first). A wait in
     the thread then sees the socket readable, its read fails, and libxrdp
     gives up; dropping makes every libxrdp wait give up too. */
  /* The thread ends the session, saying goodbye (libxrdp's Disconnect
     Provider Ultimatum: a deliberate disconnect, so the client doesn't
     connect again by itself). dropping also ends any libxrdp wait it's
     in. Only if that doesn't happen (a wait that doesn't look) is the
     socket shut down, which ends any wait. */
  dropping = 1;
  run_until_done ();
  if (dropping)
    {
      sck = server_client_socket ();
      if (sck >= 0)
        shutdown (sck, SHUT_RDWR);
      run_until_done ();
    }
}

#ifndef RDPS_MODULE
/* The command-line version (Linux; RISC OS -cli): the same thread,
   driven every centisecond (step_gap_cs: longer, for tests), as the
   module's callback drives it. */
int
server_run (const server_options *o)
{
  static size_t size = 1024 * 1024;
  void *stack = malloc (size);
  unsigned start = plat_time_cs ();
  unsigned gap = o->step_gap_cs ? o->step_gap_cs : 1;

  if (!stack || rdps_thread_init (stack, size) != 0)
    return 1;
  if (rdps_cmd_start (o) != 0)
    return 1;
  while (!server_stop)
    {
      unsigned now = plat_time_cs ();
      if (o->max_seconds && now - start >= (unsigned) o->max_seconds * 100)
        break;
      if (server_shutdown_seen)
        {
          server_shutdown_seen = 0;
          rdps_cmd_shutdown_complete ();
        }
      if (server_prereset)
        {
          /* SIGUSR2: a pretend reset (the module's Service_PreReset):
             the client is told, then the process ends without closing
             the session, as a reset would. */
          int i;
          rdps_cmd_restarting ();
          for (i = 0; i < 100; i++)
            {
              rdps_thread_run ();
              if (rdps_cmd_restart_done ())
                break;
              usleep (10000);
            }
          fprintf (stderr, "RDPServer: pretend reset\n");
          fflush (stderr);
          fflush (stdout);
          _exit (0);
        }
      if (rdps_thread_due (now))
        rdps_thread_run ();
      usleep (gap * 10000);
    }
  rdps_cmd_stop ();
  return 0;
}
#endif
