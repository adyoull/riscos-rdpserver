/* RDPServer: the login screen (see login.h).
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdlib.h>
#include <string.h>
#include "font8x8.h"
#include "kbdlayout.h"
#include "login.h"

/* TBGR: red in the low byte */
#define RGB(r, g, b) ((uint32_t) (r) | ((uint32_t) (g) << 8) | ((uint32_t) (b) << 16))
#define C_BACK   RGB (0x2a, 0x33, 0x45)
#define C_PANEL  RGB (0xee, 0xee, 0xee)
#define C_EDGE   RGB (0x77, 0x77, 0x77)
#define C_TEXT   RGB (0x10, 0x10, 0x10)
#define C_FIELD  RGB (0xff, 0xff, 0xff)
#define C_FOCUS  RGB (0x20, 0x60, 0xd0)
#define C_BUTTON RGB (0xd8, 0xd8, 0xd8)
#define C_WRONG  RGB (0xc0, 0x10, 0x10)

static void
box (login_ui *l, int x, int y, int w, int h, uint32_t c)
{
  int i, j;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > l->w) w = l->w - x;
  if (y + h > l->h) h = l->h - y;
  for (j = 0; j < h; j++)
    {
      uint32_t *p = l->px + (size_t) (y + j) * (size_t) l->w + (size_t) x;
      for (i = 0; i < w; i++)
        p[i] = c;
    }
}

static void
frame (login_ui *l, int x, int y, int w, int h, int t, uint32_t c)
{
  box (l, x, y, w, t, c);
  box (l, x, y + h - t, w, t, c);
  box (l, x, y, t, h, c);
  box (l, x + w - t, y, t, h, c);
}

/* Text at x, y (top-left), each font pixel s x s. Characters outside
   32-126 show as '?'. Stops at the right edge max_x. */
static void
text (login_ui *l, int x, int y, const char *s, uint32_t c, int max_x)
{
  int sc = l->scale;
  for (; *s && x + 8 * sc <= max_x; s++, x += 8 * sc)
    {
      unsigned ch = (unsigned char) *s;
      const unsigned char *g = font8x8[(ch >= 32 && ch <= 126 ? ch : '?') - 32];
      int r, b;
      for (r = 0; r < 8; r++)
        for (b = 0; b < 8; b++)
          if (g[r] & (1u << b))
            box (l, x + b * sc, y + r * sc, sc, sc, c);
    }
}

static void
draw_panel (login_ui *l)
{
  int sc = l->scale, ch = 8 * sc, f;
  char shown[LOGIN_FIELD + 1];

  l->changed = 1;
  box (l, l->px0, l->py0, l->pw, l->ph, C_PANEL);
  frame (l, l->px0, l->py0, l->pw, l->ph, 1, C_EDGE);
  text (l, l->px0 + ch, l->py0 + ch, "Log in to this RISC OS computer", C_TEXT,
        l->px0 + l->pw - ch / 2);
  for (f = 0; f < 2; f++)
    {
      int y = l->fy[f], n, max = (l->fw - ch) / ch;
      const char *v = f ? l->pass : l->user;
      text (l, l->px0 + ch, y + (l->fh - ch) / 2, f ? "Password" : "User name", C_TEXT, l->fx);
      box (l, l->fx, y, l->fw, l->fh, C_FIELD);
      frame (l, l->fx, y, l->fw, l->fh, l->field == f ? 2 : 1, l->field == f ? C_FOCUS : C_EDGE);
      n = (int) strlen (v);
      if (f)
        {
          memset (shown, '*', (size_t) n);
          shown[n] = 0;
        }
      else
        strcpy (shown, v);
      /* the end of a long one */
      if (n > max - 1 && max > 1)
        memmove (shown, shown + n - (max - 1), (size_t) (max - 1) + 1);
      if (l->field == f)
        strcat (shown, "_");
      text (l, l->fx + ch / 2, y + (l->fh - ch) / 2, shown, C_TEXT, l->fx + l->fw - ch / 2);
    }
  box (l, l->bx, l->by, l->bw, l->bh, C_BUTTON);
  frame (l, l->bx, l->by, l->bw, l->bh, 1, C_EDGE);
  text (l, l->bx + (l->bw - 6 * ch) / 2, l->by + (l->bh - ch) / 2, "Log in", C_TEXT, l->bx + l->bw);
  if (l->message[0])
    text (l, l->px0 + ch, l->py0 + 9 * ch, l->message, C_WRONG, l->px0 + l->pw - ch / 2);
}

int
login_open (login_ui *l, int w, int h, const char *user, int keyboard)
{
  int sc, ch;
  memset (l, 0, sizeof *l);
  if (w < 64 || h < 64)
    return -1;
  l->px = malloc ((size_t) w * (size_t) h * 4);
  if (!l->px)
    return -1;
  l->w = w;
  l->h = h;
  l->keyboard = keyboard;
  if (user)
    {
      strncpy (l->user, user, LOGIN_FIELD - 1);
      l->user[LOGIN_FIELD - 1] = 0;
    }
  l->field = l->user[0] ? 1 : 0;
  /* big text where there's room (a 320 x 256 mode gets the small font) */
  l->scale = sc = w >= 640 && h >= 400 ? 2 : 1;
  ch = 8 * sc;
  l->pw = 36 * ch < w - 16 ? 36 * ch : w - 16;
  l->ph = 15 * ch < h - 16 ? 15 * ch : h - 16;
  l->px0 = (w - l->pw) / 2;
  l->py0 = (h - l->ph) / 2;
  l->fx = l->px0 + 11 * ch;
  l->fw = l->px0 + l->pw - ch - l->fx;
  l->fh = ch * 2;
  l->fy[0] = l->py0 + 3 * ch;
  l->fy[1] = l->py0 + 6 * ch;
  l->bw = 8 * ch;
  l->bh = ch * 2;
  l->bx = l->px0 + l->pw - ch - l->bw;
  l->by = l->py0 + 11 * ch + ch / 2;
  box (l, 0, 0, w, h, C_BACK);
  draw_panel (l);
  return 0;
}

int
login_notice (login_ui *l, int w, int h, const char *title, const char *line)
{
  int sc, ch;
  memset (l, 0, sizeof *l);
  if (w < 64 || h < 64)
    return -1;
  l->px = malloc ((size_t) w * (size_t) h * 4);
  if (!l->px)
    return -1;
  l->w = w;
  l->h = h;
  l->scale = sc = w >= 640 && h >= 400 ? 2 : 1;
  ch = 8 * sc;
  l->pw = 36 * ch < w - 16 ? 36 * ch : w - 16;
  l->ph = 6 * ch < h - 16 ? 6 * ch : h - 16;
  l->px0 = (w - l->pw) / 2;
  l->py0 = (h - l->ph) / 2;
  box (l, 0, 0, w, h, C_BACK);
  box (l, l->px0, l->py0, l->pw, l->ph, C_PANEL);
  frame (l, l->px0, l->py0, l->pw, l->ph, 1, C_EDGE);
  text (l, l->px0 + ch, l->py0 + ch, title, C_TEXT, l->px0 + l->pw - ch / 2);
  text (l, l->px0 + ch, l->py0 + 3 * ch, line, C_TEXT, l->px0 + l->pw - ch / 2);
  l->changed = 1;
  return 0;
}

void
login_close (login_ui *l)
{
  if (l->px)
    {
      memset (l->pass, 0, sizeof l->pass);
      free (l->px);
    }
  memset (l, 0, sizeof *l);
}

void
login_message (login_ui *l, const char *t)
{
  strncpy (l->message, t ? t : "", sizeof l->message - 1);
  l->message[sizeof l->message - 1] = 0;
  draw_panel (l);
}

void
login_clear_password (login_ui *l)
{
  memset (l->pass, 0, sizeof l->pass);
  l->field = l->user[0] ? 1 : 0;
  draw_panel (l);
}

/* Adds a character (Latin-1) to the field with the focus. */
static void
type (login_ui *l, unsigned ch)
{
  char *v = l->field ? l->pass : l->user;
  size_t n = strlen (v);
  if (ch < 32 || (ch > 126 && ch < 160) || ch > 255 || n >= LOGIN_FIELD - 1)
    return;
  v[n] = (char) ch;
  v[n + 1] = 0;
}

static int
key_action (login_ui *l, int code)
{
  char *v = l->field ? l->pass : l->user;
  switch (code)
    {
    case 0x0E:                              /* Backspace */
      if (*v)
        v[strlen (v) - 1] = 0;
      return 0;
    case 0x0F:                              /* Tab */
      l->field ^= 1;
      return 0;
    case 0x01:                              /* Escape: empty the field */
      memset (v, 0, LOGIN_FIELD);
      return 0;
    case 0x1C:                              /* Return (and keypad Enter) */
      if (l->field == 0)
        {
          l->field = 1;
          return 0;
        }
      return LOGIN_SUBMIT;
    }
  return -1;
}

int
login_scancode (login_ui *l, int code, int ext, int up)
{
  int r;
  if (code == 0x2A || code == 0x36)
    {
      l->shift = !up;
      return 0;
    }
  if (code == 0x38 && ext)                  /* right Alt: AltGr */
    {
      l->altgr = !up;
      return 0;
    }
  if (up)
    return 0;
  if (code == 0x3A)
    {
      l->caps = !l->caps;
      return 0;
    }
  r = key_action (l, code);
  if (r < 0)
    {
      if (ext && code != 0x35)              /* the arrows and so on */
        return 0;
      if (ext)                              /* keypad / */
        type (l, '/');
      else
        type (l, kbd_char (l->keyboard, code, l->shift, l->altgr, l->caps));
      r = 0;
    }
  draw_panel (l);
  return r;
}

int
login_unicode (login_ui *l, unsigned ch, int up)
{
  int r = 0;
  if (up)
    return 0;
  if (ch == 8)
    r = key_action (l, 0x0E);
  else if (ch == 9)
    r = key_action (l, 0x0F);
  else if (ch == 13)
    r = key_action (l, 0x1C);
  else
    type (l, ch);
  draw_panel (l);
  return r;
}

int
login_click (login_ui *l, int x, int y)
{
  int f;
  for (f = 0; f < 2; f++)
    if (x >= l->fx && x < l->fx + l->fw && y >= l->fy[f] && y < l->fy[f] + l->fh)
      {
        l->field = f;
        draw_panel (l);
        return 0;
      }
  if (x >= l->bx && x < l->bx + l->bw && y >= l->by && y < l->by + l->bh)
    return LOGIN_SUBMIT;
  return 0;
}
