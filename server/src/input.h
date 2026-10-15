/* RDPServer: input from the client, paced for RISC OS.

   RDP clients send a click as "move here" and "button down" in quick
   succession, often in the same packet. RISC OS records a button change
   with the pointer's position, and the pointer only follows the mouse at
   the next VSync, so a button pressed straight after a move can land at
   the old position. And a down and up in the same instant can be lost.
   So moves are passed on at once, and each button or key change waits
   until the pointer has caught up and the last change has had time to
   register.

   A mouse button must never be left down while the server lets other
   programs run if the client has already let go of it. The Filer, given
   a double-click, loops reading the mouse (OS_Mouse, without polling the
   Wimp) and if the button is still down after a delay takes it as
   "double-click and hold", which it treats as a Shift-double-click
   (Filer s/Clicks): an application opens as a folder, a file loads into
   !Edit. That is what test1 to test3 did. So a button-down waits until
   its button-up has arrived, then the two go as one click: s->click
   presses the button and has a ticker event let go of it INQ_CLICK_CS
   later, whoever has the machine then. The Wimp must see the button down
   at a poll, so the down and up can't simply go together (test4 did that:
   no clicks at all). A button held longer than INQ_HOLD_CS, or moved
   with (a drag), goes down on its own.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_INPUT_H
#define RDPS_INPUT_H

#define INQ_MAX 256
#define INQ_KEYS 128

/* Centiseconds a button waits after a move, and between changes. */
#define INQ_AFTER_MOVE_CS 3
#define INQ_BETWEEN_CS 2
/* Centiseconds a button-down waits for its up before going down alone. */
#define INQ_HOLD_CS 25
/* Centiseconds from a click's down to its up (done by a ticker event). */
#define INQ_CLICK_CS 6
/* Pixels a button can move while down and still be a click, not a drag. */
#define INQ_DRAG_PX 4

typedef struct
{
  void (*move) (void *ctx, int x, int y);       /* pixels, top-left origin */
  void (*key) (void *ctx, int ll, int down);    /* low-level key number */
  void (*wheel) (void *ctx, int dz, int dw);    /* notches: dz < 0 is up */
  unsigned (*now) (void *ctx);                  /* the clock, centiseconds */
  /* A whole click: button down now, up INQ_CLICK_CS later by itself.
     0 = done; -1 = can't (then down and up go separately). May be NULL. */
  int (*click) (void *ctx, int ll);
  /* A byte typed (in RISC OS's alphabet), in order with the keys: 0, or
     -1 if there's no room now (it's offered again later). May be NULL. */
  int (*text) (void *ctx, int c);
  void *ctx;
} inq_sink;

typedef struct
{
  int kind, a, b;
  int seen;                         /* t is when inq_run first saw it */
  unsigned t;
} inq_event;

typedef struct
{
  inq_event ev[INQ_MAX];
  int head, count;
  unsigned last_move, last_change;
  int moved, changed;               /* last_move / last_change are valid */
  unsigned char wanted[INQ_KEYS];   /* state after everything queued */
  unsigned char applied[INQ_KEYS];  /* state RISC OS has been given */
  int wheel_acc, hwheel_acc;        /* RDP rotation units not yet a notch */
  int px, py;                       /* the last move passed on */
  int holding;                      /* a button is down and its up queued:
                                       don't return until it's up */
  int clicked;                      /* a click went: the caller should let
                                       the Wimp see it before anything else */
  int plain;                        /* buttons as keys, as they come (no
                                       clicks by ticker): for comparing */
  int click_cs;                     /* a click's down to its up (INQ_CLICK_CS) */
} inq;

void inq_init (inq *q);
/* A byte of a character the client sent as itself (RDP's Unicode
   keyboard event, already in RISC OS's alphabet): typed into the
   keyboard buffer in its turn. */
void inq_char (inq *q, int c);

/* Queue input. Repeated key-downs (clients repeat held keys; RISC OS
   repeats on its own) and ups for keys that aren't down are dropped. */
void inq_move (inq *q, int x, int y);
void inq_key (inq *q, int ll, int down);
/* RDP wheel rotation: 120 units a notch, positive away from the user
   (vertical) or to the right (horizontal). */
void inq_wheel (inq *q, int rotation, int horizontal);

/* Passes on what's due by time now (centiseconds). Returns the number of
   events still waiting. While a button is down and its up is queued, it
   waits (with s->now) rather than returning, at most INQ_BETWEEN_CS a
   change. */
int inq_run (inq *q, unsigned now, const inq_sink *s);

/* Drops what's waiting and releases everything RISC OS has been told is
   down, at once (the client has gone, or resynchronised). */
void inq_release_all (inq *q, const inq_sink *s);

#endif
