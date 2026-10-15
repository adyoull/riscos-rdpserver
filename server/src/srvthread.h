/* RDPServer: the server thread (see coro.h).

   server.c's server_start, server_step and server_finish run inside the
   coroutine, driven from outside: on RISC OS by the module's callback
   (and its ByteV handler while an error box is open), on Linux by
   server_run's loop. Commands from the front end (start, stop, drop the
   client, reload the logins) are handled at the top of the thread's
   loop; the calls below resume the thread until it has done them.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_SRVTHREAD_H
#define RDPS_SRVTHREAD_H

#include <stddef.h>
#include "server.h"

/* Sets the thread up on the given stack (at least 64 KB). 0 or -1. */
int rdps_thread_init (void *stack, size_t size);

/* The thread was lost (a resume never came back): forget it and its
   server (keys and buttons let go, nothing freed); rdps_thread_init can
   be called again. */
void rdps_thread_reset (void);

/* Driver: 1 if the thread wants to run now (its wait is over, or a
   command is waiting), given the monotonic time in cs. */
int rdps_thread_due (unsigned now);

/* Driver: runs the thread until it next waits. Does nothing if called
   from inside it (a nested callback). */
void rdps_thread_run (void);

/* When the thread next wants to run (monotonic cs). */
unsigned rdps_thread_wake_time (void);

/* Commands, from outside the thread. o is copied by the caller's
   front end layer; it must stay valid while the server runs. */
int rdps_cmd_start (const server_options *o);   /* 0, or 1: see the messages */
void rdps_cmd_stop (void);
int rdps_cmd_reload_logins (void);              /* 0 or -1 */
int rdps_cmd_set_mute (int on);                 /* 0 or -1 */
int rdps_cmd_set_require_tls (int on);          /* 0 or -1 */
int rdps_cmd_set_sound (int on);                /* 0 or -1 */
void rdps_cmd_restarting (void);                /* RISC OS about to reset */
void rdps_cmd_shutdown_complete (void);         /* the desktop has shut down */
int rdps_cmd_restart_done (void);               /* the client has been told */

/* Drops the client at once, whatever it's doing (also mid-handshake):
   the socket is shut down here, so a wait in the thread ends, and the
   thread ends the session. */
void rdps_cmd_disconnect (void);

/* Inside the thread: wait cs centiseconds (other programs run
   meanwhile). Outside (a front end call): returns at once on RISC OS,
   sleeps on Linux. */
void rdps_wait_cs (unsigned cs);

/* Centiseconds the thread has spent waiting (other programs running), in
   all: server_step's "longest step" leaves them out. */
extern unsigned rdps_yielded_cs;

/* os_calls.c: the client's send buffer size (bytes, 0 if the default),
   and bytes sent to clients in all (the module looks for progress). */
extern int rdps_sndbuf;
extern unsigned rdps_bytes_sent;
extern int rdps_sndbuf_want;

/* 1 while the client is being dropped or the server stopped: libxrdp's
   waits end (server.c's is_term). */
int rdps_thread_ending (void);

/* Whether the thread's server is running (listening). */
int rdps_thread_running (void);

#endif
