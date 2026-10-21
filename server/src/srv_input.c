/* RDPServer: input from the client: keys, the mouse, characters, to RISC
   OS (input.h's queue), and libxrdp's callback.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include "srv.h"


/* --- input ---------------------------------------------------------------- */

void
sink_move (void *ctx, int x, int y)
{
  server *s = ctx;
  plat_mouse_to (&s->scr, x, y);
}

static int loggable (int ll);

/* Before a mouse button goes down: if RISC OS thinks Shift, Ctrl or Alt
   is held but the client hasn't pressed one, let go of it, or the click
   is taken as a Shift-click (on a Pi 4 every Select acted as
   Shift-Select with no Shift from the client). A key-up for a key that's
   already up does nothing, so this can't upset anything. */
static void
unstick_modifiers (server *s)
{
  static const struct { int bit, left, right; const char *name; } mod[] = {
    { PLAT_SHIFT, 0x4C, 0x58, "Shift" },
    { PLAT_CTRL, 0x3B, 0x61, "Ctrl" },
    { PLAT_ALT, 0x5E, 0x60, "Alt" } };
  int m = plat_modifiers ();
  size_t i;

  if (s->verbose)
    say ("before the click: RISC OS has%s%s%s held, keyboard status &%02X",
         (m & PLAT_SHIFT) ? " Shift" : "", (m & PLAT_CTRL) ? " Ctrl" : "",
         (m & PLAT_ALT) ? " Alt" : (m & 7) ? "" : " nothing", (unsigned) (m >> 8) & 0xff);
  for (i = 0; i < sizeof mod / sizeof mod[0]; i++)
    if ((m & mod[i].bit) && !s->input.applied[mod[i].left]
        && !s->input.applied[mod[i].right])
      {
        say ("%s was held on RISC OS though the client hadn't pressed it: "
             "letting go of it", mod[i].name);
        plat_key (mod[i].left, 0);
        plat_key (mod[i].right, 0);
      }
}

void
sink_key (void *ctx, int ll, int down)
{
  server *s = ctx;
  if (down && ll >= 0x70 && ll <= 0x72)
    unstick_modifiers (s);
  if (s->verbose)
    {
      if (loggable (ll))
        say ("KeyV %s &%02X", down ? "down" : "up", ll);
      else
        say ("KeyV %s (a key)", down ? "down" : "up");
    }
  plat_key (ll, down);
}

int
sink_click (void *ctx, int ll)
{
  server *s = ctx;
  unstick_modifiers (s);
  if (plat_click (ll, (unsigned) s->input.click_cs) != 0)
    return -1;
  if (s->verbose)
    say ("click &%02X: KeyV down, up in %d cs by ticker", ll, s->input.click_cs);
  return 0;
}

int
sink_text (void *ctx, int c)
{
  (void) ctx;
  return plat_char (c);
}

unsigned
sink_now (void *ctx)
{
  (void) ctx;
  return plat_time_cs ();
}

void
sink_wheel (void *ctx, int dz, int dw)
{
  server *s = ctx;
  if (s->verbose)
    say ("wheel %d, %d", dz, dw);
  plat_wheel (dz, dw);
}

/* Nothing must stay pressed on RISC OS when the client goes. */
void
release_all (server *s)
{
  inq_release_all (&s->input, &s->sink);
}

/* If the program stops for any other reason, let go of the keys first. */
void
at_exit (void)
{
  plat_click_finish ();
  release_all (&srv);
  plat_set_clients (0);
}

void
fatal_signal (int sig)
{
  plat_click_finish ();
  release_all (&srv);
  signal (sig, SIG_DFL);
  raise (sig);
}

/* RDP's 9-bit wheel rotation: bit 8 is the sign. */
static int
wheel_rotation (int flags)
{
  int r = flags & 0x1ff;
  return (r & 0x100) ? r - 0x200 : r;
}

void
mouse (server *s, int flags, int x, int y)
{
  int down = (flags & PTRFLAGS_DOWN) != 0;

  if (s->verbose && (flags & ~PTRFLAGS_MOVE))
    say ("mouse flags &%04X at %d,%d", (unsigned) flags, x, y);
  if (flags & PTRFLAGS_WHEEL)
    {
      inq_wheel (&s->input, wheel_rotation (flags), 0);
      return;
    }
  if (flags & PTRFLAGS_HWHEEL)
    {
      inq_wheel (&s->input, wheel_rotation (flags), 1);
      return;
    }
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= s->scr.width) x = s->scr.width - 1;
  if (y >= s->scr.height) y = s->scr.height - 1;
  inq_move (&s->input, x, y);
  /* A PC mouse on RISC OS: left Select, middle Menu, right Adjust. */
  if (flags & PTRFLAGS_BUTTON1)
    inq_key (&s->input, 0x70, down);
  if (flags & PTRFLAGS_BUTTON3)
    inq_key (&s->input, 0x71, down);
  if (flags & PTRFLAGS_BUTTON2)
    inq_key (&s->input, 0x72, down);
}

/* Shift, Ctrl, Alt, Caps Lock, Num Lock and the mouse buttons: the keys
   -v names in the log. Other keys are logged without saying which, so a
   password typed over RDP never ends up in the log. */
static int
loggable (int ll)
{
  switch (ll)
    {
    case 0x4C: case 0x58: case 0x3B: case 0x61: case 0x5E: case 0x60:
    case 0x5D: case 0x22: case 0x70: case 0x71: case 0x72:
      return 1;
    }
  return 0;
}

static void
scancode (server *s, int flags, int code)
{
  int ll = keymap_lookup (code, (flags & KBD_FLAG_EXT) != 0);
  if (s->verbose && loggable (ll))
    say ("key scancode &%02X flags &%04X -> &%02X", (unsigned) code,
         (unsigned) flags, (unsigned) ll);
  else if (s->verbose && ll < 0)
    say ("key scancode &%02X flags &%04X: no RISC OS key", (unsigned) code,
         (unsigned) flags);
  if (ll >= 0)
    inq_key (&s->input, ll, (flags & KBD_FLAG_UP) == 0);
}

/* libxrdp's callback: input from the client, and its requests. */
int
callback (intptr_t id, int msg, intptr_t param1, intptr_t param2,
          intptr_t param3, intptr_t param4)
{
  server *s = (server *) id;
  if (s == NULL || s->session == NULL)
    return 0;
  if ((s->restarting || s->restart_due)
      && (msg == RDP_INPUT_SYNCHRONIZE || msg == RDP_INPUT_SCANCODE
          || msg == RDP_INPUT_UNICODE || msg == RDP_INPUT_MOUSE
          || msg == RDP_INPUT_MOUSEX))
    return 0;                   /* RISC OS is about to reset: no more input */
  /* The desktop has shut down and its box offers only Restart: a click
     or Return from the client is most likely that, and the reset would
     come before the page could go. So this one is held back (its up and
     anything after it dropped), the page goes, then it goes as a whole
     click (or Return), then our side of the connection is shut. */
  if (s->shutdown_done && s->was_up && !s->in_login
      && ((msg == RDP_INPUT_MOUSE && ((int) param3 & PTRFLAGS_DOWN)
           && ((int) param3 & (PTRFLAGS_BUTTON1 | PTRFLAGS_BUTTON2 | PTRFLAGS_BUTTON3)))
          || (msg == RDP_INPUT_SCANCODE && !((int) param3 & KBD_FLAG_UP) && param1 == 0x1C)))
    {
      int f = (int) param3;
      if (msg == RDP_INPUT_SCANCODE)
        s->restart_ll = keymap_lookup (0x1C, (f & KBD_FLAG_EXT) != 0);
      else
        s->restart_ll = (f & PTRFLAGS_BUTTON1) ? 0x70
                        : (f & PTRFLAGS_BUTTON3) ? 0x71 : 0x72;   /* Select, Menu, Adjust */
      if (msg == RDP_INPUT_MOUSE)
        mouse (s, (int) param1, (int) param2, f & ~(PTRFLAGS_DOWN | PTRFLAGS_BUTTON1
                                                    | PTRFLAGS_BUTTON2 | PTRFLAGS_BUTTON3));
      s->restart_due = 1;
      s->restart_by_input = 1;
      return 0;
    }
  s->last_activity = plat_time_cs ();
  switch (msg)
    {
    case RDP_INPUT_SCANCODE:
      if (s->in_login)
        {
          /* to the login screen, not RISC OS (and not logged: a password) */
          if (login_scancode (&s->login, (int) param1, ((int) param3 & KBD_FLAG_EXT) != 0,
                              ((int) param3 & KBD_FLAG_UP) != 0) == LOGIN_SUBMIT)
            s->login_due = 1;
          break;
        }
      scancode (s, (int) param3, (int) param1);
      break;
    case RDP_INPUT_UNICODE:
      if (s->in_login)
        {
          if (login_unicode (&s->login, (unsigned) param1, ((int) param3 & KBD_FLAG_UP) != 0)
              == LOGIN_SUBMIT)
            s->login_due = 1;
          break;
        }
      /* A character sent as itself, not as a key (Windows App on a Mac,
         an on-screen keyboard, an input method): typed into the keyboard
         buffer, in its turn with the keys. (Which character isn't
         logged.) */
      if (!((int) param3 & KBD_FLAG_UP))
        {
          unsigned char b[4];
          int n = kbd_unicode_bytes ((unsigned) param1, s->alphabet, b), i;
          if (n > 0)
            {
              if (!s->unicode_said)
                say ("the client sends characters as themselves (Unicode): typing them "
                     "into the keyboard buffer (RISC OS alphabet %d)", s->alphabet);
              s->unicode_said = 1;
              for (i = 0; i < n; i++)
                inq_char (&s->input, b[i]);
            }
          else if (!s->unicode_bad_said)
            {
              s->unicode_bad_said = 1;
              say ("the client sent a character RISC OS's alphabet hasn't got: not typed");
            }
        }
      break;
    case RDP_INPUT_SYNCHRONIZE:
      /* Sent when the client window gets the focus back: any key-ups
         sent while it hadn't got it are lost, so let go of everything
         (the client sends key-downs for keys still held). */
      if (s->verbose)
        say ("synchronise (lock keys &%X)", (unsigned) param1);
      release_all (s);
      break;
    case RDP_INPUT_MOUSE:
      if (s->in_login)
        {
          if (((int) param3 & PTRFLAGS_BUTTON1) && ((int) param3 & PTRFLAGS_DOWN)
              && login_click (&s->login, (int) param1, (int) param2) == LOGIN_SUBMIT)
            s->login_due = 1;
          break;
        }
      mouse (s, (int) param3, (int) param1, (int) param2);
      break;
    case 0x5559:   /* output suppressed (param1 1) or allowed again (0) */
      if (param1 == 0 && s->cap.frame)
        {
          cap_mark_all (&s->cap);
          s->palette_due = s->client_bpp == 8;
          s->pointer_due = 1;
        }
      break;
    case 0x5555:   /* channel data: MAKELONG (channel id, flags), size, data, total */
      sound_channel_data (&s->snd, (int) (param1 & 0xffff), (int) ((param1 >> 16) & 0xffff),
                          (const void *) param3, (int) param2, (int) param4);
      break;
    case 0x4444:   /* the client asks for a rectangle to be redrawn */
      if (s->cap.frame)
        cap_mark (&s->cap, (int) param1, (int) param2, (int) param3, (int) param4);
      break;
    default:
      break;
    }
  return 0;
}
