/* RDPServer: input from the client, paced for RISC OS. See input.h.
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <string.h>
#include "input.h"

enum { EV_MOVE = 1, EV_KEY, EV_WHEEL };

void
inq_init (inq *q)
{
  memset (q, 0, sizeof *q);
}

static inq_event *
slot (inq *q, int i)
{
  return &q->ev[(q->head + i) % INQ_MAX];
}

static void
push (inq *q, int kind, int a, int b)
{
  inq_event *e;
  if (q->count == INQ_MAX)
    {
      /* Full (a client flooding us): drop the oldest. */
      q->head = (q->head + 1) % INQ_MAX;
      q->count--;
    }
  e = slot (q, q->count++);
  e->kind = kind;
  e->a = a;
  e->b = b;
}

void
inq_move (inq *q, int x, int y)
{
  /* Moves in a row: only the last matters. */
  if (q->count > 0 && slot (q, q->count - 1)->kind == EV_MOVE)
    {
      slot (q, q->count - 1)->a = x;
      slot (q, q->count - 1)->b = y;
      return;
    }
  push (q, EV_MOVE, x, y);
}

void
inq_key (inq *q, int ll, int down)
{
  if (ll < 0 || ll >= INQ_KEYS)
    return;
  down = down != 0;
  if (q->wanted[ll] == down)
    return;
  q->wanted[ll] = (unsigned char) down;
  push (q, EV_KEY, ll, down);
}

/* Whole notches out of the accumulated rotation, rounding towards zero. */
static int
notches (int *acc, int rotation)
{
  int n;
  *acc += rotation;
  n = *acc / 120;
  *acc -= n * 120;
  return n;
}

void
inq_wheel (inq *q, int rotation, int horizontal)
{
  int n;
  if (horizontal)
    {
      n = notches (&q->hwheel_acc, rotation);
      if (n)
        push (q, EV_WHEEL, 0, n);
    }
  else
    {
      n = notches (&q->wheel_acc, rotation);
      if (n)
        push (q, EV_WHEEL, -n, 0);   /* RISC OS: negative is up/away */
    }
}

int
inq_run (inq *q, unsigned now, const inq_sink *s)
{
  while (q->count > 0)
    {
      inq_event *e = slot (q, 0);
      if (e->kind == EV_KEY)
        {
          int is_button = e->a >= 0x70 && e->a <= 0x74;
          if (is_button && q->moved
              && (int) (now - q->last_move) < INQ_AFTER_MOVE_CS)
            break;
          if (q->changed && (int) (now - q->last_change) < INQ_BETWEEN_CS)
            break;
          s->key (s->ctx, e->a, e->b);
          q->applied[e->a] = (unsigned char) e->b;
          q->last_change = now;
          q->changed = 1;
        }
      else if (e->kind == EV_MOVE)
        {
          s->move (s->ctx, e->a, e->b);
          q->last_move = now;
          q->moved = 1;
        }
      else
        s->wheel (s->ctx, e->a, e->b);
      q->head = (q->head + 1) % INQ_MAX;
      q->count--;
    }
  return q->count;
}

void
inq_release_all (inq *q, const inq_sink *s)
{
  int i;
  q->head = q->count = 0;
  for (i = 0; i < INQ_KEYS; i++)
    {
      if (q->applied[i])
        s->key (s->ctx, i, 0);
      q->applied[i] = q->wanted[i] = 0;
    }
  q->wheel_acc = q->hwheel_acc = 0;
}
