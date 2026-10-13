/* RDPServer: input from the client, paced for RISC OS.

   RDP clients send a click as "move here" and "button down" in quick
   succession, often in the same packet. RISC OS records a button change
   with the pointer's position, and the pointer only follows the mouse at
   the next VSync, so a button pressed straight after a move can land at
   the old position. And a down and up in the same instant can be lost.
   So moves are passed on at once, and each button or key change waits
   until the pointer has caught up and the last change has had time to
   register.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_INPUT_H
#define RDPS_INPUT_H

#define INQ_MAX 256
#define INQ_KEYS 128

/* Centiseconds a button waits after a move, and between changes. */
#define INQ_AFTER_MOVE_CS 3
#define INQ_BETWEEN_CS 2

typedef struct
{
  void (*move) (void *ctx, int x, int y);       /* pixels, top-left origin */
  void (*key) (void *ctx, int ll, int down);    /* low-level key number */
  void (*wheel) (void *ctx, int dz, int dw);    /* notches: dz < 0 is up */
  void *ctx;
} inq_sink;

typedef struct
{
  int kind, a, b;
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
} inq;

void inq_init (inq *q);

/* Queue input. Repeated key-downs (clients repeat held keys; RISC OS
   repeats on its own) and ups for keys that aren't down are dropped. */
void inq_move (inq *q, int x, int y);
void inq_key (inq *q, int ll, int down);
/* RDP wheel rotation: 120 units a notch, positive away from the user
   (vertical) or to the right (horizontal). */
void inq_wheel (inq *q, int rotation, int horizontal);

/* Passes on what's due by time now (centiseconds). Returns the number of
   events still waiting. */
int inq_run (inq *q, unsigned now, const inq_sink *s);

/* Drops what's waiting and releases everything RISC OS has been told is
   down, at once (the client has gone, or resynchronised). */
void inq_release_all (inq *q, const inq_sink *s);

#endif
