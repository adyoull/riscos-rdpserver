/* RDPServer: input from the client, paced for RISC OS. See input.h.
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <string.h>
#include "input.h"

enum { EV_DONE = 0, EV_MOVE = 1, EV_KEY, EV_WHEEL, EV_CHAR };

void
inq_init (inq *q)
{
  memset (q, 0, sizeof *q);
  q->click_cs = INQ_CLICK_CS;
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
  e->seen = 0;
  e->t = 0;
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

void
inq_char (inq *q, int c)
{
  if (c >= 0 && c <= 255)
    push (q, EV_CHAR, c, 0);
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

static int
is_button (int ll)
{
  return ll >= 0x70 && ll <= 0x72;
}

/* Where the up for button ll is queued after event i, or -1. */
static int
up_at (inq *q, int i, int ll)
{
  for (i++; i < q->count; i++)
    {
      inq_event *e = slot (q, i);
      if (e->kind == EV_KEY && e->a == ll && e->b == 0)
        return i;
    }
  return -1;
}

static int
up_queued (inq *q, int i, int ll)
{
  return up_at (q, i, ll) >= 0;
}

/* Is a move more than INQ_DRAG_PX from the pointer queued after event i? */
static int
drag_queued (inq *q, int i)
{
  for (i++; i < q->count; i++)
    {
      inq_event *e = slot (q, i);
      if (e->kind == EV_MOVE
          && (e->a - q->px > INQ_DRAG_PX || q->px - e->a > INQ_DRAG_PX
              || e->b - q->py > INQ_DRAG_PX || q->py - e->b > INQ_DRAG_PX))
        return 1;
    }
  return 0;
}

/* Can the down at event 0 and its up at u go as one click? Only if what
   comes between them is small moves (or nothing): otherwise it's a drag,
   or other keys whose order matters. */
static int
clean_click (inq *q, int u)
{
  int i;
  for (i = 1; i < u; i++)
    {
      inq_event *e = slot (q, i);
      if (e->kind == EV_DONE)
        continue;
      if (e->kind != EV_MOVE
          || e->a - q->px > INQ_DRAG_PX || q->px - e->a > INQ_DRAG_PX
          || e->b - q->py > INQ_DRAG_PX || q->py - e->b > INQ_DRAG_PX)
        return 0;
    }
  return 1;
}

int
inq_run (inq *q, unsigned now, const inq_sink *s)
{
  while (q->count > 0)
    {
      inq_event *e = slot (q, 0);
      if (e->kind == EV_KEY)
        {
          /* plain: buttons as keys, down and up as they come (as before
             0.2-test4); otherwise a button-down waits for its up and goes
             as one click */
          int button_down = is_button (e->a) && e->b;
          int press = button_down && !q->plain;
          if (press)
            {
              if (!e->seen)
                {
                  e->seen = 1;
                  e->t = now;
                }
              /* Wait for its up (or a drag, or a long hold). */
              if (!up_queued (q, 0, e->a) && !drag_queued (q, 0)
                  && (int) (now - e->t) < INQ_HOLD_CS)
                break;
            }
          /* Pacing. Nothing is held yet for a button-down's own wait, so
             returning is fine; once a button is down with its up queued,
             wait here instead. */
          while ((button_down && q->moved && (int) (now - q->last_move) < INQ_AFTER_MOVE_CS)
                 || (q->changed && (int) (now - q->last_change) < INQ_BETWEEN_CS))
            {
              if (!q->holding || !s->now)
                return q->count;
              now = s->now (s->ctx);
            }
          /* A click whose up is here: one click, the up by ticker. The
             next change waits until that up has happened. */
          if (press && s->click && up_queued (q, 0, e->a)
              && clean_click (q, up_at (q, 0, e->a))
              && s->click (s->ctx, e->a) == 0)
            {
              slot (q, up_at (q, 0, e->a))->kind = EV_DONE;
              /* Not down as far as release_all is concerned: the ticker
                 (or plat_click_finish) lets go of it. */
              q->applied[e->a] = 0;
              q->clicked = 1;
              q->last_change = now + (unsigned) q->click_cs + 1;
              q->changed = 1;
              q->head = (q->head + 1) % INQ_MAX;
              q->count--;
              continue;
            }
          s->key (s->ctx, e->a, e->b);
          q->applied[e->a] = (unsigned char) e->b;
          q->last_change = now;
          q->changed = 1;
          if (press && up_queued (q, 0, e->a))
            q->holding = e->a;
          else if (is_button (e->a) && !e->b && q->holding == e->a)
            q->holding = 0;
        }
      else if (e->kind != EV_DONE && q->changed
               && (int) (now - q->last_change) < 0)
        /* A click's up is still to come: the pointer and wheel keep
           still until then. */
        return q->count;
      else if (e->kind == EV_MOVE)
        {
          s->move (s->ctx, e->a, e->b);
          q->px = e->a;
          q->py = e->b;
          q->last_move = now;
          q->moved = 1;
        }
      else if (e->kind == EV_WHEEL)
        s->wheel (s->ctx, e->a, e->b);
      else if (e->kind == EV_CHAR && s->text && s->text (s->ctx, e->a) != 0)
        return q->count;                /* the keyboard buffer is full */
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
  q->holding = 0;
}
