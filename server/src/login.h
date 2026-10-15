/* RDPServer: the login screen.

   When a login is set and a client connects without the right user name
   and password (Windows' own client, say, which can't ask for them), the
   session still comes up, but the client sees this page instead of the
   RISC OS screen: it's drawn into a 32 bpp TBGR canvas of the screen's
   size, which the server sends in the screen's place. Keys and clicks go
   here, not to RISC OS. Nothing of the RISC OS screen is read or sent
   until the login is right.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_LOGIN_H
#define RDPS_LOGIN_H

#include <stddef.h>
#include <stdint.h>

#define LOGIN_FIELD 64          /* user name and password, with the 0 */

typedef struct
{
  int w, h;                     /* the canvas: the screen's size */
  uint32_t *px;                 /* w * h pixels, 32 bpp TBGR (red low) */
  char user[LOGIN_FIELD];
  char pass[LOGIN_FIELD];
  int field;                    /* 0: user name, 1: password */
  int changed;                  /* drawn since the server last looked */
  int shift, caps, altgr;       /* Shift and AltGr held, Caps Lock on */
  int keyboard;                 /* RISC OS keyboard layout (kbdlayout.h) */
  char message[80];             /* shown under the fields (a wrong login) */
  /* layout: the panel, the two fields and the button */
  int scale, px0, py0, pw, ph;
  int fx, fw, fh, fy[2], bx, by, bw, bh;
} login_ui;

/* The page for a w x h screen, with the user name the client sent (may
   be ""), typing with RISC OS keyboard layout keyboard (kbdlayout.h).
   -1 if there's no memory. */
int login_open (login_ui *l, int w, int h, const char *user, int keyboard);
void login_close (login_ui *l);

/* A notice in the login page's place, with no fields or button: a title
   and a line under it, on the same background and panel (the computer
   restarting, 0.2-test24). -1 if there's no memory. */
int login_notice (login_ui *l, int w, int h, const char *title, const char *line);
/* A line under the fields ("" for none). */
void login_message (login_ui *l, const char *text);
/* The fields again, from the start (after a wrong password). */
void login_clear_password (login_ui *l);

/* Input. Each returns 1 when the user asks to log in (Return, or the
   button), else 0. Scan codes are RDP's (set 1), ext for the E0 keys. */
#define LOGIN_SUBMIT 1
int login_scancode (login_ui *l, int code, int ext, int up);
int login_unicode (login_ui *l, unsigned ch, int up);
int login_click (login_ui *l, int x, int y);

#endif
