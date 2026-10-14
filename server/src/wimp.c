/* RDPServer: the desktop front end. See wimp.h.

   Block layouts: Wimp_CreateWindow (window block, 88 bytes, then 32
   bytes per icon), Wimp_CreateIcon, Wimp_CreateMenu (28-byte header, 24
   bytes per item), Wimp_PollIdle (PRM p741), Wimp_SetIconState,
   Wimp_GetWindowState, Wimp_SetCaretPosition, Wimp_ProcessKey,
   Wimp_GetPointerInfo, Wimp_ReportError.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>

#include "platform.h"
#include "auth.h"
#include "server.h"
#include "wimp.h"

#define APP_NAME "RDPServer"
#define TASK_WORD 0x4B534154          /* "TASK" */
#define WIMP_VERSION 310

/* Icon flags (PRM, Wimp_CreateIcon). */
#define IF_TEXT 0x1
#define IF_SPRITE 0x2
#define IF_BORDER 0x4
#define IF_HCENTRE 0x8
#define IF_VCENTRE 0x10
#define IF_FILLED 0x20
#define IF_INDIRECTED 0x100
#define IF_SELECTED 0x200000
#define IF_SHADED 0x400000
#define IF_CLICK (3 << 12)
#define IF_WRITABLE (15 << 12)
#define IF_COLOURS(fg, bg) (((fg) << 24) | ((bg) << 28))

#define LABEL (IF_TEXT | IF_VCENTRE | IF_INDIRECTED | IF_COLOURS (7, 1))
#define FIELD (IF_TEXT | IF_BORDER | IF_VCENTRE | IF_FILLED | IF_INDIRECTED | IF_COLOURS (7, 0))
#define WRITABLE (FIELD | IF_WRITABLE)
#define BUTTON (IF_TEXT | IF_BORDER | IF_HCENTRE | IF_VCENTRE | IF_FILLED \
                | IF_INDIRECTED | IF_CLICK | IF_COLOURS (7, 1))

/* An option icon: the optoff/opton sprites at the left, the text after
   (text and sprite, vertically centred: PRM icon flags). Its state is
   toggled here on a click (button type Click). */
#define OPTION (IF_TEXT | IF_SPRITE | IF_VCENTRE | IF_INDIRECTED | IF_CLICK \
                | IF_COLOURS (7, 1))

/* Polling: no pointer entering/leaving or caret gained/lost events. */
#define POLL_MASK ((1 << 4) | (1 << 5) | (1 << 11) | (1 << 12))

enum { W_STATUS, W_CHOICES, W_INFO, W_COUNT };
#define MAX_ICONS 11
#define TEXT_SIZE 160

/* Status window lines. */
enum { ST_STATE, ST_LOGIN, ST_SENT, ST_HEADING, ST_MSG0, ST_LINES = ST_MSG0 + 6 };
/* Choices icons. */
enum { CH_PORT_L, CH_PORT, CH_USER_L, CH_USER, CH_PASS_L, CH_PASS, CH_NOTE,
       CH_CLEAR, CH_CANCEL, CH_SAVE, CH_MUTE, CH_COUNT };

typedef struct
{
  int x0, y0, x1, y1;
  unsigned flags;
  const char *text;            /* the starting text */
  const char *valid;           /* validation string, or NULL */
  int size;                    /* buffer size the Wimp is told (0: TEXT_SIZE) */
} icon_def;

static struct
{
  int handle;
  int nicons;
  char text[MAX_ICONS][TEXT_SIZE];
  char valid[MAX_ICONS][16];
  char title[48];
} win[W_COUNT];

static const wimp_setup *setup;
static int task;
static int running;            /* the server is listening */
static char port_text[8];
static int mute_shown;          /* the Choices option's state */
static char bar_sprite[13] = "!rdpserver";
static int bar_icon = -1, bar_on;
static unsigned next_refresh;
static FILE *log_out;
static int refresh_due;

/* The last few messages, for the Status window. */
#define RING 6
static char ring[RING][TEXT_SIZE];
static int ring_next;

static struct
{
  char title[12];
  unsigned char colours[4];    /* title fg, bg; work fg, bg */
  int width, height, gap;
  struct { int flags, submenu, icon_flags; char text[12]; } item[5];
} menu = {
  APP_NAME, { 7, 2, 7, 0 }, 12 * 16, 44, 0,
  {
    { 0, -1, 0x07000021, "Info" },
    { 0, -1, 0x07000021, "Status\x8c" },
    { 0, -1, 0x07000021, "Choices\x8c" },
    { 0, -1, 0x07000021, "Disconnect" },
    { 0x80, -1, 0x07000021, "Quit" }
  }
};
enum { M_INFO, M_STATUS, M_CHOICES, M_DISCONNECT, M_QUIT };

/* --- small things ---------------------------------------------------------- */

void
wimp_report (const char *msg)
{
  _kernel_oserror e;
  e.errnum = 0;
  snprintf (e.errmess, sizeof e.errmess, "%s", msg);
  _swix (Wimp_ReportError, _INR (0, 2), &e, 1 /* OK box */, APP_NAME);
}

static int
fail (const _kernel_oserror *e)
{
  if (e)
    wimp_report (e->errmess);
  return e != NULL;
}

/* Changes an icon's text and redraws it (only if it changed, unless
   force: a buffer cleared behind the Wimp's back still shows the old
   text). */
static void
put_text (int w, int i, const char *text, int force)
{
  int b[4];
  if (!force && strcmp (win[w].text[i], text) == 0)
    return;
  snprintf (win[w].text[i], TEXT_SIZE, "%s", text);
  if (win[w].handle == -1)
    return;
  b[0] = win[w].handle; b[1] = i; b[2] = 0; b[3] = 0;   /* change nothing: redraw */
  _swix (Wimp_SetIconState, _IN (1), b);
}

static void forget_password (void);

static void
set_text (int w, int i, const char *text)
{
  put_text (w, i, text, 0);
}

const char *
wimp_icon_text (int w, int i)
{
  return win[w].text[i];
}

int
wimp_shows_connected (void)
{
  return bar_on;
}

/* The server's messages: the log file and the Status window. */
static void
message (const char *line)
{
  if (log_out)
    {
      fprintf (log_out, "%s\n", line);
      fflush (log_out);
    }
  snprintf (ring[ring_next], TEXT_SIZE, "%s", line);
  ring_next = (ring_next + 1) % RING;
  refresh_due = 1;
}

int
wimp_read_port (const char *file, int def)
{
  FILE *f = fopen (file, "r");
  char line[64];
  int port = def, p;
  if (!f)
    return def;
  while (fgets (line, sizeof line, f))
    if (sscanf (line, "port %d", &p) == 1 && p >= 1 && p <= 65535)
      port = p;
  fclose (f);
  return port;
}

/* "mute_local 0|1": mute this computer while a client listens. */
int
wimp_read_mute (const char *file, int def)
{
  FILE *f = fopen (file, "r");
  char line[64];
  int on = def, v;
  if (!f)
    return def;
  while (fgets (line, sizeof line, f))
    if (sscanf (line, "mute_local %d", &v) == 1 && (v == 0 || v == 1))
      on = v;
  fclose (f);
  return on;
}

int
wimp_write_settings (const char *file, int port, int mute_local)
{
  FILE *f = fopen (file, "w");
  if (!f)
    return -1;
  fprintf (f, "port %d\nmute_local %d\n", port, mute_local != 0);
  return fclose (f) == 0 ? 0 : -1;
}

/* --- building the windows -------------------------------------------------- */

static int
create_window (int w, const char *title, int x, int y, int width, int height,
               unsigned flags, const icon_def *icons, int n)
{
  int b[22 + 8 * MAX_ICONS];
  int i;

  memset (b, 0, sizeof b);
  win[w].nicons = n;
  snprintf (win[w].title, sizeof win[w].title, "%s", title);
  b[0] = x; b[1] = y - height; b[2] = x + width; b[3] = y;   /* on screen */
  b[6] = -1;
  b[7] = (int) flags;
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;   /* title */
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 1;   /* work area */
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;   /* scroll bars */
  ((unsigned char *) b)[38] = 12;                                  /* title with the caret */
  b[10] = 0; b[11] = -height; b[12] = width; b[13] = 0;            /* work area */
  b[14] = IF_TEXT | IF_BORDER | IF_HCENTRE | IF_VCENTRE | IF_FILLED | IF_INDIRECTED;
  b[15] = 0;
  b[16] = 1;
  b[18] = (int) (intptr_t) win[w].title; b[19] = -1; b[20] = (int) sizeof win[w].title;
  b[21] = n;
  for (i = 0; i < n; i++)
    {
      int *ic = &b[22 + 8 * i];
      snprintf (win[w].text[i], TEXT_SIZE, "%s", icons[i].text);
      ic[0] = icons[i].x0; ic[1] = icons[i].y0; ic[2] = icons[i].x1; ic[3] = icons[i].y1;
      ic[4] = (int) icons[i].flags;
      ic[5] = (int) (intptr_t) win[w].text[i];
      if (icons[i].valid)
        {
          snprintf (win[w].valid[i], sizeof win[w].valid[i], "%s", icons[i].valid);
          ic[6] = (int) (intptr_t) win[w].valid[i];
        }
      else
        ic[6] = -1;
      ic[7] = icons[i].size > 0 && icons[i].size <= TEXT_SIZE ? icons[i].size : TEXT_SIZE;
    }
  return fail (_swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &win[w].handle));
}

/* Window flags: new format, title, movable, the Wimp redraws it; with a
   back and close icon for the ordinary windows. */
#define WF_PLAIN 0x84000012u
#define WF_CLOSE 0x87000012u

static int
create_status (void)
{
  icon_def ic[ST_LINES];
  int i;
  for (i = 0; i < ST_LINES; i++)
    {
      ic[i].x0 = 16; ic[i].x1 = 1184;
      ic[i].y1 = -16 - 44 * i; ic[i].y0 = ic[i].y1 - 40;
      ic[i].flags = LABEL;
      ic[i].text = i == ST_HEADING ? "Last messages:" : "";
      ic[i].valid = NULL;
    }
  return create_window (W_STATUS, "RDPServer status", 240, 900, 1200,
                        32 + 44 * ST_LINES, WF_CLOSE, ic, ST_LINES);
}

static int
create_choices (void)
{
  static const icon_def ic[CH_COUNT] = {
    { 16, -68, 240, -20, LABEL, "Port", NULL, 0 },
    { 256, -68, 416, -20, WRITABLE, "3389", "Ktar;A0-9", 6 },
    { 16, -124, 240, -76, LABEL, "User name", NULL, 0 },
    { 256, -124, 784, -76, WRITABLE, "", "Ktar;A~ ", AUTH_NAME },
    { 16, -180, 240, -132, LABEL, "Password", NULL, 0 },
    { 256, -180, 784, -132, WRITABLE, "", "D*;Ktar", 64 },
    { 16, -236, 784, -188, LABEL, "", NULL, 0 },
    { 16, -372, 240, -304, BUTTON, "Clear login", "R5,3", 0 },
    { 400, -372, 576, -304, BUTTON, "Cancel", "R5,3", 0 },
    { 592, -372, 784, -304, BUTTON, "Save", "R6,3", 0 },
    { 16, -292, 784, -244, OPTION, "Mute this computer while a client listens",
      "Soptoff,opton", 0 }
  };
  return create_window (W_CHOICES, "RDPServer choices", 400, 800, 800, 388,
                        WF_CLOSE, ic, CH_COUNT);
}

static int
create_info (void)
{
  icon_def ic[8] = {
    { 16, -64, 200, -20, LABEL, "Name", NULL, 0 },
    { 216, -64, 784, -20, FIELD, "RDPServer", "R2", 0 },
    { 16, -116, 200, -72, LABEL, "Purpose", NULL, 0 },
    { 216, -116, 784, -72, FIELD, "Remote desktop (RDP) server", "R2", 0 },
    { 16, -168, 200, -124, LABEL, "Author", NULL, 0 },
    { 216, -168, 784, -124, FIELD, "\xa9 Andrew Youll, 2026", "R2", 0 },
    { 16, -220, 200, -176, LABEL, "Version", NULL, 0 },
    { 216, -220, 784, -176, FIELD, "", "R2", 0 }
  };
  ic[7].text = setup->version;
  return create_window (W_INFO, "About this program", 400, 800, 800, 236,
                        WF_PLAIN, ic, 8);
}

static int
create_iconbar_icon (void)
{
  int b[9];
  memset (b, 0, sizeof b);
  b[0] = -1;                          /* the right of the icon bar */
  b[1] = 0; b[2] = 0; b[3] = 68; b[4] = 68;
  b[5] = IF_SPRITE | IF_HCENTRE | IF_VCENTRE | IF_INDIRECTED | IF_CLICK;
  b[6] = (int) (intptr_t) bar_sprite; /* sprite name, in the Wimp's pool */
  b[7] = 1;
  b[8] = (int) sizeof bar_sprite;
  return fail (_swix (Wimp_CreateIcon, _INR (0, 1) | _OUT (0), 0, b, &bar_icon));
}

/* --- what the windows show -------------------------------------------------- */

static void
login_note (void)
{
  auth_db db;
  char t[TEXT_SIZE];
  if (auth_load (setup->logins_file, &db) == 0 && db.count > 0)
    snprintf (t, sizeof t, "Clients log in as %.60s.", db.login[0].name);
  else
    snprintf (t, sizeof t, "No login set: anyone on your network can connect.");
  set_text (W_CHOICES, CH_NOTE, t);
}

static void
refresh (void)
{
  server_status st;
  char t[TEXT_SIZE];
  int i, on;

  server_get_status (&st);
  if (!running)
    snprintf (t, sizeof t, "Not listening: choose another port in Choices.");
  else if (st.connected)
    snprintf (t, sizeof t, "Connected: %.40s (%.30s), %d x %d, %d bpp", st.client,
              st.user[0] ? st.user : "no user name", st.width, st.height, st.bpp);
  else if (st.connecting)
    snprintf (t, sizeof t, "Connecting: %.63s", st.client);
  else
    snprintf (t, sizeof t, "Waiting for a client on port %s", port_text);
  set_text (W_STATUS, ST_STATE, t);
  set_text (W_STATUS, ST_LOGIN, st.login_set ? "Clients must give the user name and password set in Choices."
                                              : "No login set: anyone on your network can connect.");
  snprintf (t, sizeof t, "Screen pieces sent: %u", st.tiles_sent);
  set_text (W_STATUS, ST_SENT, t);
  for (i = 0; i < RING; i++)
    set_text (W_STATUS, ST_MSG0 + i, ring[(ring_next + i) % RING]);

  on = st.connected || st.connecting;
  if (on != bar_on && bar_icon != -1)
    {
      int b[4] = { -2, bar_icon, 0, 0 };
      bar_on = on;
      snprintf (bar_sprite, sizeof bar_sprite, "%s", on ? "rdpserver-on" : "!rdpserver");
      _swix (Wimp_SetIconState, _IN (1), b);
    }
}

static void
open_window (int w)
{
  int b[9];
  b[0] = win[w].handle;
  if (fail (_swix (Wimp_GetWindowState, _IN (1), b)))
    return;
  b[7] = -1;                              /* bring it to the front */
  fail (_swix (Wimp_OpenWindow, _IN (1), b));
}

/* Shows the option on or off. */
static void
show_mute (int on)
{
  int b[4];
  mute_shown = on != 0;
  if (win[W_CHOICES].handle == -1)
    return;
  b[0] = win[W_CHOICES].handle; b[1] = CH_MUTE;
  b[2] = mute_shown ? IF_SELECTED : 0; b[3] = IF_SELECTED;   /* EOR, clear */
  _swix (Wimp_SetIconState, _IN (1), b);
}

static void
fill_choices (void)
{
  auth_db db;
  set_text (W_CHOICES, CH_PORT, port_text);
  show_mute (setup->opt->mute_local);
  if (auth_load (setup->logins_file, &db) == 0 && db.count > 0)
    set_text (W_CHOICES, CH_USER, db.login[0].name);
  else
    set_text (W_CHOICES, CH_USER, "");
  forget_password ();
  login_note ();
}

static void
open_choices (void)
{
  fill_choices ();
  open_window (W_CHOICES);
  _swix (Wimp_SetCaretPosition, _INR (0, 5), win[W_CHOICES].handle, CH_PORT,
         0, 0, -1, (int) strlen (port_text));
}

/* --- the server ------------------------------------------------------------- */

static void
start_server (void)
{
  setup->opt->port = port_text;
  running = server_start (setup->opt) == 0;
  if (!running)
    {
      char t[200];
      const char *why = server_start_error ();
      if (why)
        snprintf (t, sizeof t, "%s", why);
      else
        snprintf (t, sizeof t, "RDPServer can't listen on port %s (is another "
                  "server using it?). Choose another port in Choices.", port_text);
      wimp_report (t);
    }
  refresh_due = 1;
}

/* Clears the password (and the copy the Wimp shows), and puts the caret
   at the start of the field if it was in it. */
static void
forget_password (void)
{
  int caret[6];
  memset (win[W_CHOICES].text[CH_PASS], 0, TEXT_SIZE);
  put_text (W_CHOICES, CH_PASS, "", 1);
  if (win[W_CHOICES].handle != -1
      && _swix (Wimp_GetCaretPosition, _IN (1), caret) == NULL
      && caret[0] == win[W_CHOICES].handle && caret[1] == CH_PASS)
    _swix (Wimp_SetCaretPosition, _INR (0, 5), win[W_CHOICES].handle, CH_PASS,
           0, 0, -1, 0);
}

/* Save: 0 if the window can close. */
static int
save_choices (void)
{
  const char *port = win[W_CHOICES].text[CH_PORT];
  const char *user = win[W_CHOICES].text[CH_USER];
  const char *pass = win[W_CHOICES].text[CH_PASS];
  char *end;
  long p = strtol (port, &end, 10);
  auth_db db;
  int have = auth_load (setup->logins_file, &db) == 0 && db.count > 0;

  if (end == port || *end || p < 1 || p > 65535)
    {
      wimp_report ("The port must be a number from 1 to 65535 (RDP's own is 3389).");
      return 1;
    }
  if (pass[0] && !user[0])
    {
      wimp_report ("Give a user name for the password too.");
      return 1;
    }
  if (user[0] && !pass[0] && !(have && strcmp (db.login[0].name, user) == 0))
    {
      wimp_report ("Give a password for that user name (or Clear login to "
                   "let anyone connect).");
      return 1;
    }
  if (user[0] && pass[0])
    {
      if (auth_set (setup->logins_file, user, pass) != 0)
        {
          wimp_report ("The login couldn't be saved (a user name has no spaces "
                       "and is under 64 characters).");
          return 1;
        }
      server_reload_logins ();
      message ("login saved: clients must now log in");
    }
  forget_password ();
  if (mute_shown != setup->opt->mute_local)
    {
      setup->opt->mute_local = mute_shown;
      if (running)
        server_set_mute (mute_shown);
      message (mute_shown ? "this computer is muted while a client listens"
                          : "this computer plays sound while a client listens");
      if (p == atoi (port_text) && running
          && wimp_write_settings (setup->settings_file, (int) p, mute_shown) != 0)
        wimp_report ("The choice couldn't be saved in Choices: it's used until "
                     "RDPServer quits.");
    }
  /* A new port, or the same one again after it couldn't be used. */
  if (p != atoi (port_text) || !running)
    {
      if (wimp_write_settings (setup->settings_file, (int) p, setup->opt->mute_local) != 0)
        wimp_report ("The port couldn't be saved in Choices: it's used until "
                     "RDPServer quits.");
      if (running)
        server_finish ();
      snprintf (port_text, sizeof port_text, "%ld", p);
      start_server ();
    }
  return 0;
}

static void
clear_login (void)
{
  remove (setup->logins_file);
  server_reload_logins ();
  message ("login cleared: anyone can connect");
  put_text (W_CHOICES, CH_USER, "", 1);
  forget_password ();
  if (win[W_CHOICES].handle != -1)
    _swix (Wimp_SetCaretPosition, _INR (0, 5), win[W_CHOICES].handle, CH_USER,
           0, 0, -1, 0);
  login_note ();
}

static void
close_choices (void)
{
  forget_password ();
  _swix (Wimp_CloseWindow, _IN (1), &win[W_CHOICES].handle);
}

/* --- events ----------------------------------------------------------------- */

static void
show_menu (int x)
{
  server_status st;
  int i;
  server_get_status (&st);
  for (i = 0; i < 5; i++)
    menu.item[i].icon_flags &= ~IF_SHADED;
  if (!st.connected && !st.connecting)
    menu.item[M_DISCONNECT].icon_flags |= IF_SHADED;
  menu.item[M_INFO].submenu = win[W_INFO].handle;
  fail (_swix (Wimp_CreateMenu, _INR (1, 3), &menu, x - 64, 96 + 5 * 44));
}

static int last_menu_x;

int
wimp_event (int reason, int *b)
{
  switch (reason)
    {
    case Open_Window_Request:
      fail (_swix (Wimp_OpenWindow, _IN (1), b));
      break;
    case Close_Window_Request:
      if (b[0] == win[W_CHOICES].handle)
        close_choices ();
      else
        fail (_swix (Wimp_CloseWindow, _IN (1), b));
      break;
    case Mouse_Click:
      if (b[3] == -2)                      /* the icon bar */
        {
          if (b[2] == Button_Select)
            {
              refresh ();
              open_window (W_STATUS);
            }
          else if (b[2] == Button_Adjust)
            open_choices ();
          else if (b[2] == Button_Menu)
            {
              last_menu_x = b[0];
              show_menu (b[0]);
            }
        }
      else if (b[3] == win[W_CHOICES].handle && b[2] != Button_Menu)
        {
          if (b[4] == CH_SAVE)
            {
              if (save_choices () == 0)
                {
                  if (b[2] == Button_Adjust)
                    fill_choices ();      /* Adjust: save, keep it open */
                  else
                    close_choices ();
                }
            }
          else if (b[4] == CH_CANCEL)
            {
              if (b[2] == Button_Adjust)
                fill_choices ();          /* Adjust: back to what's saved */
              else
                close_choices ();
            }
          else if (b[4] == CH_CLEAR)
            clear_login ();
          else if (b[4] == CH_MUTE)
            show_mute (!mute_shown);
        }
      break;
    case Key_Pressed:
      if (b[0] == win[W_CHOICES].handle && b[6] == 13)
        {
          if (save_choices () == 0)
            close_choices ();
        }
      else if (b[0] == win[W_CHOICES].handle && b[6] == 0x1b)
        close_choices ();
      else
        _swix (Wimp_ProcessKey, _IN (0), b[6]);
      break;
    case Menu_Selection:
      {
        int pointer[5];
        switch (b[0])
          {
          case M_STATUS:
            refresh ();
            open_window (W_STATUS);
            break;
          case M_CHOICES:
            open_choices ();
            break;
          case M_DISCONNECT:
            server_disconnect ();
            refresh_due = 1;
            break;
          case M_QUIT:
            return 1;
          }
        /* Adjust keeps the menu open (Style Guide). */
        if (_swix (Wimp_GetPointerInfo, _IN (1), pointer) == NULL
            && (pointer[2] & Button_Adjust))
          show_menu (last_menu_x);
      }
      break;
    case User_Message:
    case User_Message_Recorded:
      if (b[4] == Message_Quit)
        return 1;
      break;
    }
  return 0;
}

unsigned
wimp_tick (void)
{
  unsigned wait = running ? (unsigned) server_step (0) : 25;
  unsigned now = plat_time_cs ();
  server_status st;

  server_get_status (&st);
  if ((st.connected || st.connecting) != bar_on || refresh_due
      || (int) (now - next_refresh) >= 0)
    {
      refresh ();
      refresh_due = 0;
      next_refresh = now + 50;
    }
  return now + wait;
}

int
wimp_init (const wimp_setup *w)
{
  static const int messages[] = { 0 };   /* only Message_Quit */
  int i;

  setup = w;
  for (i = 0; i < W_COUNT; i++)
    win[i].handle = -1;
  bar_icon = -1;
  bar_on = 0;
  running = 0;
  ring_next = 0;
  memset (ring, 0, sizeof ring);
  snprintf (bar_sprite, sizeof bar_sprite, "!rdpserver");
  snprintf (port_text, sizeof port_text, "%d", wimp_read_port (w->settings_file, 3389));
  w->opt->mute_local = wimp_read_mute (w->settings_file, 0);
  mute_shown = w->opt->mute_local;
  log_out = w->log_file ? fopen (w->log_file, "w") : NULL;
  server_set_message_hook (message);

  if (fail (_swix (Wimp_Initialise, _INR (0, 3) | _OUT (1),
                   WIMP_VERSION, TASK_WORD, APP_NAME, messages, &task)))
    return 1;
  if (create_status () || create_choices () || create_info () || create_iconbar_icon ())
    return 1;
  if (log_out)
    fprintf (log_out, "RDPServer %s\n", w->version);
  start_server ();
  refresh ();
  return 0;
}

void
wimp_close (void)
{
  server_shutdown ();
  running = 0;
  server_set_message_hook (NULL);
  if (log_out)
    fclose (log_out);
  log_out = NULL;
  _swix (Wimp_CloseDown, _INR (0, 1), task, TASK_WORD);
}

int
wimp_run (const wimp_setup *w)
{
  static int block[64];
  int reason, quit = 0;

  if (wimp_init (w))
    {
      wimp_close ();
      return 1;
    }
  while (!quit)
    {
      /* Sleeps until the server next needs to run, or something happens
         on the desktop: null events come no sooner than that. */
      unsigned at = wimp_tick ();
      _kernel_oserror *e = _swix (Wimp_PollIdle, _INR (0, 2) | _OUT (0), POLL_MASK,
                                  block, at, &reason);
      if (e)
        {
          wimp_report (e->errmess);
          break;
        }
      quit = wimp_event (reason, block);
    }
  wimp_close ();
  return 0;
}
