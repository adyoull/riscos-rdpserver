/* RDPServer: the server's own state and the pieces of it each file
   uses (server.c and the srv_*.c files). Not for the rest of the
   program: server.h is the server's interface.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_SRV_H
#define RDPS_SRV_H

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "arch.h"
#include "os_calls.h"
#include "parse.h"
#include "trans.h"
#include "libxrdpinc.h"
#include "ms-rdpbcgr.h"
#include "xrdp_client_info.h"
#include "log.h"

#include "platform.h"
#include "pixels.h"
#include "pointer.h"
#include "login.h"
#include "kbdlayout.h"
#include "capture.h"
#include "keymap.h"
#include "sound.h"
#include "input.h"
#include "auth.h"
#include "server.h"
#include "tuning.h"
#include "srvthread.h"
#include "tls.h"
#include "xrdp_peek.h"
#include "encspeed.h"

typedef struct
{
  const server_options *opt;
  struct trans *listener;
  struct trans *trans;             /* the client's connection, or NULL */
  struct xrdp_session *session;
  int was_up;                      /* session->up_and_running last time */
  rdps_screen scr;
  pix_format fmt;
  uint32_t pal[256];               /* the palette (2 to 256 colour modes) */
  int pal_n;
  capture cap;
  int client_bpp;
  unsigned char *tilebuf;          /* one tile in the client's depth */
  inq input;                       /* input waiting to go to RISC OS */
  inq_sink sink;
  auth_db logins;
  unsigned refuse_until;           /* after a wrong password: cs */
  unsigned resize_deadline;        /* cs; the client must be back by then */
  int palette_due;                 /* 8 bpp client reactivated: palette again */
  int pointer_due;                 /* the pointer shape to send (pointer.h) */
  /* the login screen (login.h), while the client hasn't logged in */
  login_ui login;
  int in_login, login_due, login_tries;
  unsigned login_until, login_wait;
  int go_due;                      /* logged in during a resize: session_go
                                      once the client is back */
  unsigned up_since;               /* when the client came up (cs) */
  unsigned stall_bytes, stall_since; /* sent so far, and since when output
                                      has waited with nothing going */
  unsigned last_rtt_cs;            /* the last network check's round trip */
  unsigned handshake_deadline;      /* cs; 0 once the client is up */
  unsigned tiles_sent;
  unsigned next_scan;              /* when the next band of rows is due */
  unsigned last_activity;          /* last change seen or input received */
  unsigned steps, reads, most_reads; /* for -v's figures every 5 s */
  unsigned step_cs_max, scans, stats_at;
  unsigned stats_bytes;            /* rdps_bytes_sent when the 5 s began */
  int scan_rows;                   /* most rows a turn (0: no limit) */
  int sample;                      /* read 1 row in this many a pass (1: all) */
  unsigned scan_at;                /* when the last band was read */
  unsigned long long stats_read;   /* cap.bytes_read when the 5 s began */
  unsigned pass_start, pass_cs;    /* the current pass over the screen began;
                                      the last whole one took */
  unsigned last_step_end, gap_max;  /* time between steps: other programs */
  char client_desc[64];            /* "192.168.0.58:58303" */
  char client_user[64];
  rdps_sound snd;                  /* sound to the client (sound.c) */
  int mute_local;                  /* from the options; Choices can change it */
  int require_tls;                 /* likewise */
  int send_sound;                  /* likewise: off unless chosen */
  int mem_refusing;                /* no connections taken: memory short */
  int sound_full;                  /* RDPServer$SoundRate 0: sound at the
                                      sound system's rate */
  int verbose;                     /* the detailed log: from the options, then
                                      Choices */
  int sound_stop_due;              /* Choices turned it off: stop it in step */
  int sound_closed;                /* stopped by Choices: not again this session */
  int fills_var, bulk_var;         /* RDPServer$Fills, $BulkCompress (-1: unset) */
  int fills_ok;                    /* this client takes OpaqueRect orders */
  int unicode_said, unicode_bad_said; /* logged this session */
  int alphabet;                    /* RISC OS's, read when a client comes */
  int restart_due, restarting;     /* RISC OS about to reset
                                      (Service_PreReset) */
  int restart_state;               /* 1 sending the page, 2 our side shut
                                      (FIN), 3 done */
  unsigned restart_at;
  int shutdown_done;               /* Service_ShutdownComplete: the desktop
                                      has shut down, the Restart box is up */
  int restart_by_input;            /* the page is for a click or key that
                                      may be that box's Restart */
  int restart_ll;                  /* which: &70-&72 a button (a whole
                                      click), else a key (down and up) */
  unsigned fills_sent;             /* solid rectangles sent as fills */
  const char *ph_name;             /* what the step is doing (phase ()) */
  unsigned ph_t, ph_y, slow_said;
  int running;
  /* keep-alive (keepalive_step) */
  int ka_caps;                     /* libxrdp_keepalive_caps: 1 heartbeats, 2 RTT */
  int ka_resizing;                 /* keepalive_step saw a resize under way */
  unsigned ka_next;                /* the next heartbeat and RTT request */
  unsigned last_heard;             /* the last data from the client */
  int rtt_seq, rtt_seen;           /* requests sent; replies counted */
  int rtt_ok;                      /* it has answered one: it answers them */
  unsigned rtt_sent_at;
  struct trans *waiting;           /* a new connection, held while the
                                      client is checked (conn_in) */
  unsigned waiting_until;
  unsigned check_from, check_until; /* the check: the client must answer */
  int check_alive;                 /* it did: the waiting connection is refused */
} server;

/* A client gets this long to finish connecting. libxrdp's waits during
   the connection sequence yield (srvthread.h), so other programs run
   meanwhile; this only stops a client that never finishes. */
#define HANDSHAKE_CS 3000
/* After this long without a change on the screen or input from the
   client, scan at the idle rate. */
#define ACTIVE_CS 200
/* -v logs any wait this long between steps while a client is connected. */
#define GAP_LOG_CS 25
/* Keep-alive, for clients that take it (libxrdp patch 0005: the MCS
   message channel). Every KA_PERIOD_S a Server Heartbeat PDU ([MS-RDPBCGR]
   2.2.16.1): if they stop coming (this computer hung, the network went),
   the client warns after KA_WARN missed and connects again by itself
   (auto-reconnect) after KA_RECONNECT. And an RTT Measure Request (2.2.14):
   a client that answers them and then says nothing at all for SILENT_CS
   has gone without closing its connection (asleep, off the network), and
   the session ends, so it can connect again. */
#define KA_PERIOD_S 5
#define KA_WARN 3
#define KA_RECONNECT 6
#define SILENT_CS 4500
/* A connection that comes while a client is connected waits this long
   while that client is checked: if it doesn't answer within CHECK_CS (or
   its connection turns out to be dead), it's dropped and the new one
   takes its place. */
#define WAITING_CS 700
#define CHECK_CS 500
/* A client that takes nothing of what's waiting to go to it for this long
   has gone, even if its connection is still open (a RISC OS client whose
   program stopped: its socket stays, nobody reads it). */
#define STALL_CS 6000
/* The login screen: this long to log in, this many tries, and a pause
   after each wrong one. */
#define LOGIN_CS (2 * 60 * 100)
#define LOGIN_TRIES 5
#define LOGIN_PAUSE_CS 300

#define MEMORY_RESERVE (1024 * 1024)   /* kept back for running out */

extern server srv;
extern unsigned rdps_modexp_cs;   /* ssl_calls.c */

void phase (server *s, const char *name);   /* server.c */
void say (const char *fmt, ...);   /* server.c */
int step (unsigned max_wait_cs);   /* server.c */
void copy_text (char *to, size_t size, const char *from);   /* server.c */
void release_all (server *s);   /* srv_input.c */
int sink_click (void *ctx, int ll);   /* srv_input.c */
int callback (intptr_t id, int msg, intptr_t param1, intptr_t param2, intptr_t param3, intptr_t param4);   /* srv_input.c */
void mouse (server *s, int flags, int x, int y);   /* srv_input.c */
unsigned sink_now (void *ctx);   /* srv_input.c */
void sink_wheel (void *ctx, int dz, int dw);   /* srv_input.c */
void at_exit (void);   /* srv_input.c */
void sink_move (void *ctx, int x, int y);   /* srv_input.c */
void fatal_signal (int sig);   /* srv_input.c */
void sink_key (void *ctx, int ll, int down);   /* srv_input.c */
int sink_text (void *ctx, int c);   /* srv_input.c */
int keepalive_step (server *s, unsigned now);   /* srv_keepalive.c */
void waiting_step (server *s, unsigned now);   /* srv_keepalive.c */
void neon_say (void);   /* srv_measure.c */
void screen_copy_choose (server *s);   /* srv_measure.c */
void encode_speed_say (void);   /* srv_measure.c */
void restart_deliver (server *s);   /* srv_restart.c */
void restart_notice (server *s);   /* srv_restart.c */
void palette_refresh (server *s, int redirected);   /* srv_session.c */
void session_end (server *s, const char *why);   /* srv_session.c */
int screen_resized (server *s);   /* srv_session.c */
void memory_say (const char *what);   /* srv_session.c */
int sndbuf_from_var (void);   /* srv_session.c */
void session_start (server *s, struct trans *t);   /* srv_session.c */
void server_arc_connection_ended (void);   /* srv_session.c */
void session_go (server *s, const char *user);   /* srv_session.c */
void login_try (server *s);   /* srv_session.c */
int is_term (void);   /* srv_session.c */
int session_up (server *s);   /* srv_session.c */
int conn_in (struct trans *listener, struct trans *t);   /* srv_session.c */
void check_client (server *s);   /* srv_session.c */
void send_dirty (server *s);   /* srv_updates.c */
void arc_forget (void);   /* srv_session.c */

#endif
