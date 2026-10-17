/* RDPServer: the desktop front end (src/wimp.c) on Linux, against the Fake
   RISC OS. Each Wimp SWI it uses is faked here, recording what it asked
   for; the server and the logins are stand-ins, so the test can say
   what state the server is in and check what the front end does with it.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "fake.h"
#include "swis.h"
#include "wimp.h"
#include "auth.h"
#include "platform.h"

/* --- stand-ins for the server, the logins and the platform ---------------- */

static server_status fake_status;
static int started, finished, disconnected, reloaded, steps, start_fails, shutdowns;
static const char *start_error;
static char started_port[8];
static unsigned step_wait = 25;
static void (*hook) (const char *);
static char saved_name[64], saved_pass[64];
static int have_login;

int server_start (const server_options *o)
{
  started++;
  snprintf (started_port, sizeof started_port, "%s", o->port);
  if (start_fails)
    return 1;
  fake_status.listening = 1;
  return 0;
}
int server_step (unsigned max_wait_cs) { (void) max_wait_cs; steps++; return (int) step_wait; }
void server_finish (void) { finished++; fake_status.listening = 0; }
void server_shutdown (void) { shutdowns++; fake_status.listening = 0; }
static int leaves;
void server_leave_running (void) { leaves++; }
const char *server_start_error (void) { return start_fails ? start_error : NULL; }
void server_disconnect (void) { disconnected++; fake_status.connected = 0; }
int server_reload_logins (void) { reloaded++; return 0; }
static int mute_calls, mute_set = -1;
void server_set_mute (int on) { mute_calls++; mute_set = on; }
static int tls_calls, tls_set = -1;
void server_set_require_tls (int on) { tls_calls++; tls_set = on; }
static int sound_calls, sound_set = -1;
void server_set_sound (int on) { sound_calls++; sound_set = on; }
static int verbose_calls, verbose_set = -1;
void server_set_verbose (int on) { verbose_calls++; verbose_set = on; }
static char choices[64];
const char *plat_choices_dir (void) { return choices; }
void server_get_status (server_status *st) { *st = fake_status; st->login_set = have_login; }
void server_set_message_hook (void (*h) (const char *)) { hook = h; }

int auth_load (const char *file, auth_db *db)
{
  (void) file;
  memset (db, 0, sizeof *db);
  if (have_login)
    {
      db->count = 1;
      snprintf (db->login[0].name, sizeof db->login[0].name, "%s", saved_name);
    }
  return 0;
}
int auth_check (const auth_db *db, const char *n, const char *p) { (void) db; (void) n; (void) p; return 0; }
int auth_set (const char *file, const char *name, const char *password)
{
  (void) file;
  if (strchr (name, ' '))
    return -1;
  snprintf (saved_name, sizeof saved_name, "%s", name);
  snprintf (saved_pass, sizeof saved_pass, "%s", password);
  have_login = 1;
  return 0;
}

static unsigned now = 100000;
unsigned plat_time_cs (void) { return now; }

/* --- the fake Wimp ------------------------------------------------------- */

#define FIRST_WINDOW 0x2a000
static int windows, window_icons[8];
static int *window_blocks[8];
static int bar_block[9], bar_icon_calls;
static int opened_window, open_calls, closed_window;
static int redraws, last_redraw[4], bar_redraws, opt_state[4], tls_state[4];
static int sound_state[4], mute_shade[4];
static const char *menu_ptr;
static int menu_x, menu_y, menu_calls;
static char reported[256];
static int caret[6], processed_key = -1, pointer_buttons;
static int store[8][22 + 8 * 16];

static const _kernel_oserror *wimp (int swi, int r[10])
{
  int *b = (int *) r[1];
  switch (swi)
    {
    case Wimp_Initialise:
      CHECK (r[1] == 0x4B534154 && strcmp ((const char *) r[2], "RDPServer") == 0,
             "Wimp_Initialise: \"TASK\", RDPServer");
      r[0] = 380; r[1] = 0x1234;
      return NULL;
    case Wimp_CreateIcon:
      memcpy (bar_block, b, sizeof bar_block);
      bar_icon_calls++;
      r[0] = 3;
      return NULL;
    case Wimp_CreateWindow:
      memcpy (store[windows], b, sizeof (int) * (size_t) (22 + 8 * (b[21] < 16 ? b[21] : 16)));
      window_blocks[windows] = store[windows];
      window_icons[windows] = b[21];
      r[0] = FIRST_WINDOW + windows++;
      return NULL;
    case Wimp_GetWindowState:
      return NULL;
    case Wimp_OpenWindow:
      opened_window = b[0];
      open_calls++;
      return NULL;
    case Wimp_CloseWindow:
      closed_window = b[0];
      return NULL;
    case Wimp_SetIconState:
      memcpy (last_redraw, b, sizeof last_redraw);
      if (b[1] == 10 && b[3] == 0x200000)
        memcpy (opt_state, b, sizeof opt_state);
      if (b[1] == 10 && b[3] == 0x400000)
        memcpy (mute_shade, b, sizeof mute_shade);
      if (b[1] == 11)
        memcpy (tls_state, b, sizeof tls_state);
      if (b[1] == 12)
        memcpy (sound_state, b, sizeof sound_state);
      redraws++;
      if (b[0] == -2 && b[1] == 3)
        bar_redraws++;
      return NULL;
    case Wimp_CreateMenu:
      menu_ptr = (const char *) b;
      menu_x = r[2]; menu_y = r[3];
      menu_calls++;
      return NULL;
    case Wimp_SetCaretPosition:
      memcpy (caret, r, sizeof caret);
      return NULL;
    case Wimp_GetCaretPosition:
      b[0] = caret[0]; b[1] = caret[1]; b[5] = caret[5];
      return NULL;
    case Wimp_ProcessKey:
      processed_key = r[0];
      return NULL;
    case Wimp_GetPointerInfo:
      b[2] = pointer_buttons;
      return NULL;
    case Wimp_ReportError:
      snprintf (reported, sizeof reported, "%s", ((const _kernel_oserror *) r[0])->errmess);
      return NULL;
    case Wimp_CloseDown:
      return NULL;
    }
  return NULL;
}

static void fake_wimp (void)
{
  static const struct { int swi; const char *name; } list[] = {
    { Wimp_Initialise, "Wimp_Initialise" }, { Wimp_CreateIcon, "Wimp_CreateIcon" },
    { Wimp_CreateWindow, "Wimp_CreateWindow" }, { Wimp_GetWindowState, "Wimp_GetWindowState" },
    { Wimp_OpenWindow, "Wimp_OpenWindow" }, { Wimp_CloseWindow, "Wimp_CloseWindow" },
    { Wimp_SetIconState, "Wimp_SetIconState" }, { Wimp_CreateMenu, "Wimp_CreateMenu" },
    { Wimp_SetCaretPosition, "Wimp_SetCaretPosition" }, { Wimp_GetCaretPosition, "Wimp_GetCaretPosition" }, { Wimp_ProcessKey, "Wimp_ProcessKey" },
    { Wimp_GetPointerInfo, "Wimp_GetPointerInfo" },
    { Wimp_ReportError, "Wimp_ReportError" }, { Wimp_CloseDown, "Wimp_CloseDown" } };
  size_t i;
  fake_reset ();
  for (i = 0; i < sizeof list / sizeof list[0]; i++)
    fake_swi (list[i].swi, list[i].name, wimp);
  windows = 0;
}

enum { STATUS = FIRST_WINDOW, CHOICES, INFO };

static int click (int window, int icon, int buttons)
{
  int b[64] = { 0 };
  b[0] = 1000; b[1] = 20; b[2] = buttons; b[3] = window; b[4] = icon;
  return wimp_event (Mouse_Click, b);
}

static int key (int window, int k)
{
  int b[64] = { 0 };
  b[0] = window; b[1] = 1; b[6] = k;
  return wimp_event (Key_Pressed, b);
}

static int choose (int item)
{
  int b[64] = { 0 };
  b[0] = item; b[1] = -1;
  return wimp_event (Menu_Selection, b);
}

/* Types into a Choices icon, as the Wimp would (writes its buffer). */
static void type (int icon, const char *text)
{
  int *ic = &window_blocks[1][22 + 8 * icon];
  snprintf ((char *) ic[5], (size_t) ic[7], "%s", text);
}

static const char *menu_item (int i)
{
  return menu_ptr + 28 + 24 * i + 12;
}

static int menu_item_flags (int i)
{
  const int *it = (const int *) (menu_ptr + 28 + 24 * i);
  return it[2];
}

int main (void)
{
  char dir[] = "/tmp/rdps-wimp-XXXXXX", settings[64], log[64], line[256];
  server_options o;
  wimp_setup w;
  FILE *f;
  int b[64] = { 0 };

  if (!mkdtemp (dir))
    return 1;
  snprintf (settings, sizeof settings, "%s/Settings", dir);
  snprintf (log, sizeof log, "%s/Log", dir);
  snprintf (choices, sizeof choices, "%s", dir);
  memset (&o, 0, sizeof o);
  w.opt = &o; w.version = "0.1-test3"; w.settings_file = settings;
  w.logins_file = "unused"; w.log_file = log;

  CHECK (wimp_read_port (settings, 3389) == 3389, "no settings file: port 3389");
  CHECK (wimp_write_settings (settings, 3390, 1, 1, 1, 1) == 0 && wimp_read_port (settings, 3389) == 3390
         && wimp_read_tls (settings, 0) == 1,
         "the port is saved and read back");
  CHECK (wimp_read_mute (settings, 0) == 1, "and the mute choice");
  CHECK (wimp_read_sound (settings, 0) == 1, "and the sound choice");
  CHECK (wimp_read_log (settings, 0) == 1, "and the detailed log choice");
  CHECK (wimp_write_settings (settings, 3390, 0, 0, 0, 0) == 0 && wimp_read_mute (settings, 1) == 0
         && wimp_read_tls (settings, 1) == 0 && wimp_read_sound (settings, 1) == 0
         && wimp_read_log (settings, 1) == 0,
         "mute, sound and the detailed log off are saved too");
  f = fopen (settings, "w"); fputs ("port 99999\n", f); fclose (f);
  CHECK (wimp_read_port (settings, 3389) == 3389, "a port out of range is ignored");
  f = fopen (settings, "w"); fputs ("port 5000\n", f); fclose (f);

  fake_wimp ();
  CHECK (wimp_init (&w) == 0, "wimp_init succeeds");
  CHECK (started == 1 && strcmp (started_port, "5000") == 0, "the server starts on the saved port");
  CHECK (windows == 3, "three windows: Status, Choices, Info");
  CHECK (bar_block[0] == -1 && (bar_block[5] & 0x102) == 0x102, "an indirected sprite on the icon bar");
  CHECK (strcmp ((const char *) bar_block[6], "!rdpserver") == 0 && bar_block[7] == 1,
         "the !rdpserver sprite, from the Wimp's pool");
  CHECK (((bar_block[5] >> 12) & 15) == 3, "the icon reports clicks");
  CHECK (window_icons[1] == 14, "Choices has its fourteen icons");
  CHECK (((window_blocks[1][22 + 8 * 1 + 4] >> 12) & 15) == 15
         && ((window_blocks[1][22 + 8 * 5 + 4] >> 12) & 15) == 15, "port and password are writable");
  CHECK (strncmp ((const char *) window_blocks[1][22 + 8 * 5 + 6], "D*", 2) == 0,
         "the password shows as stars");
  CHECK (strcmp (wimp_icon_text (2, 7), "0.1-test3") == 0, "Info shows the version");
  CHECK (strstr (wimp_icon_text (0, 0), "Waiting for a client on port 5000") != NULL,
         "Status: waiting on port 5000");
  CHECK (hook != NULL, "the server's messages come to the front end");

  /* The server's messages: the log and the Status window. */
  hook ("listening on port 5000");
  wimp_tick ();
  CHECK (strcmp (wimp_icon_text (0, 27), "listening on port 5000") == 0, "the newest message is last");
  CHECK (strcmp (wimp_icon_text (0, 1), "Client") == 0 && strcmp (wimp_icon_text (0, 10), "Login") == 0,
         "Status: the details' labels");
  CHECK (strcmp (wimp_icon_text (0, 11), "-") == 0, "nothing connected: the details say -");
  f = fopen (log, "r");
  CHECK (f && fgets (line, sizeof line, f) && strstr (line, "RDPServer 0.1-test3"), "the log starts with the version");
  CHECK (f && fgets (line, sizeof line, f) && strcmp (line, "listening on port 5000\n") == 0,
         "and has the message");
  if (f) fclose (f);

  /* wimp_tick runs the server and says when to come back. */
  steps = 0; step_wait = 2;
  CHECK (wimp_tick () == now + 2 && steps == 1, "the next poll is when the server asks (2 cs)");
  step_wait = 25;
  CHECK (wimp_tick () == now + 25, "or 25 cs when it's idle");

  /* A client connects: the icon changes. */
  fake_status.connected = 1;
  snprintf (fake_status.client, sizeof fake_status.client, "192.168.0.58:58303");
  snprintf (fake_status.user, sizeof fake_status.user, "andrew");
  fake_status.width = 1920; fake_status.height = 1080; fake_status.bpp = 32;
  redraws = 0;
  wimp_tick ();
  CHECK (wimp_shows_connected (), "connected: the icon bar shows it");
  CHECK (strcmp ((const char *) bar_block[6], "rdpserver-on") == 0, "with the rdpserver-on sprite");
  CHECK (bar_redraws == 1, "the icon is redrawn");
  CHECK (strstr (wimp_icon_text (0, 0), "Connected: 192.168.0.58:58303 (andrew), 1920 x 1080, 32 bpp") != NULL,
         "Status says who's connected");
  snprintf (fake_status.hostname, sizeof fake_status.hostname, "Mac-Studio");
  snprintf (fake_status.protocol, sizeof fake_status.protocol, "RDP 10.12");
  snprintf (fake_status.security, sizeof fake_status.security, "TLSv1.2, TLS-ECDHE-RSA-WITH-CHACHA20-POLY1305-SHA256");
  snprintf (fake_status.screen, sizeof fake_status.screen, "1920 x 1080, 16 million colours");
  fake_status.connected_s = 125;
  fake_status.kbytes_sent = 20480;
  fake_status.tiles_sent = 900;
  now += 60;
  wimp_tick ();
  CHECK (strcmp (wimp_icon_text (0, 11), "192.168.0.58:58303 (Mac-Studio)") == 0, "Status: the client and its name");
  CHECK (strcmp (wimp_icon_text (0, 12), "andrew") == 0, "the user");
  CHECK (strcmp (wimp_icon_text (0, 13), "2 min 5 s") == 0, "connected for");
  CHECK (strcmp (wimp_icon_text (0, 14), "RDP 10.12") == 0, "the protocol");
  CHECK (strstr (wimp_icon_text (0, 15), "CHACHA20") != NULL, "the security");
  CHECK (strcmp (wimp_icon_text (0, 16), "1920 x 1080, 16 million colours; the client 32 bpp") == 0,
         "the screen and the client's depth");
  CHECK (strcmp (wimp_icon_text (0, 19), "20 MB, 900 screen pieces") == 0, "what's been sent");
  fake_status.connected = 0;
  fake_status.connecting = 1;
  fake_status.at_login = 1;
  now += 60;
  wimp_tick ();
  CHECK (strstr (wimp_icon_text (0, 0), "At the login screen: 192.168.0.58:58303") != NULL,
         "at the login screen: said so");
  fake_status.connected = 1;
  fake_status.connecting = 0;
  fake_status.at_login = 0;
  now += 60;
  wimp_tick ();

  /* The icon bar. */
  CHECK (click (-2, 3, Button_Select) == 0 && opened_window == STATUS, "Select opens Status");
  CHECK (click (-2, 3, Button_Menu) == 0 && menu_ptr && strcmp (menu_ptr, "RDPServer") == 0,
         "Menu opens the menu");
  CHECK (menu_x == 1000 - 64 && menu_y == 96 + 5 * 44, "above the icon");
  CHECK (strcmp (menu_item (0), "Info") == 0 && strcmp (menu_item (4), "Quit") == 0, "Info ... Quit");
  CHECK (((const int *) (menu_ptr + 28))[1] == INFO, "Info leads to the Info window");
  CHECK ((menu_item_flags (3) & 0x400000) == 0, "Disconnect can be chosen while connected");
  pointer_buttons = Button_Select;
  CHECK (choose (3) == 0 && disconnected == 1, "Disconnect disconnects");
  wimp_tick ();
  CHECK (!wimp_shows_connected (), "and the icon changes back");
  click (-2, 3, Button_Menu);
  CHECK (menu_item_flags (3) & 0x400000, "Disconnect is greyed out with no client");
  menu_calls = 0;
  pointer_buttons = Button_Adjust;
  CHECK (choose (1) == 0 && opened_window == STATUS && menu_calls == 1,
         "Status... with Adjust opens Status and keeps the menu open");
  pointer_buttons = Button_Select;

  /* Choices: the login. */
  CHECK (click (-2, 3, Button_Adjust) == 0 && opened_window == CHOICES, "Adjust opens Choices");
  CHECK (caret[0] == CHOICES && caret[1] == 1 && caret[5] == 4, "with the caret at the end of the port");
  CHECK (window_blocks[1][22 + 8 * 1 + 7] == 6, "the port field takes 5 digits");
  CHECK (strcmp (wimp_icon_text (1, 1), "5000") == 0, "showing the port");
  CHECK (strstr (wimp_icon_text (1, 6), "No login set"), "and that there's no login");
  type (3, "andrew"); type (5, "");
  reported[0] = 0;
  CHECK (click (CHOICES, 9, Button_Select) == 0 && strstr (reported, "Give a password"),
         "a user name with no password is refused");
  type (3, ""); type (5, "secret");
  reported[0] = 0;
  click (CHOICES, 9, Button_Select);
  CHECK (strstr (reported, "Give a user name"), "a password with no user name is refused");
  type (3, "andrew"); type (5, "secret");
  reported[0] = 0; closed_window = 0;
  CHECK (click (CHOICES, 9, Button_Select) == 0 && reported[0] == 0, "Save with both");
  CHECK (have_login && strcmp (saved_name, "andrew") == 0 && strcmp (saved_pass, "secret") == 0,
         "sets the login");
  CHECK (reloaded == 1, "and the server reads it again (no restart)");
  CHECK (closed_window == CHOICES, "the window closes");
  CHECK (wimp_icon_text (1, 5)[0] == 0, "and the password is forgotten");
  /* Adjust on Save: saved, the window stays, the stars go. */
  click (-2, 3, Button_Adjust);
  type (5, "secret2");
  caret[0] = CHOICES; caret[1] = 5; caret[5] = 7;
  redraws = 0; closed_window = 0;
  CHECK (click (CHOICES, 9, Button_Adjust) == 0 && closed_window == 0 && strcmp (saved_pass, "secret2") == 0,
         "Adjust on Save saves and keeps the window open");
  CHECK (wimp_icon_text (1, 5)[0] == 0 && last_redraw[0] == CHOICES && redraws > 0,
         "the password field is emptied and redrawn");
  CHECK (caret[1] == 5 && caret[5] == 0, "with the caret back at its start");
  CHECK (started == 1, "the port didn't change: the server wasn't restarted");

  click (-2, 3, Button_Adjust);
  CHECK (strcmp (wimp_icon_text (1, 3), "andrew") == 0, "Choices shows the user name");
  CHECK (strstr (wimp_icon_text (1, 6), "Clients log in as andrew"), "and that there's a login");
  type (5, "");
  closed_window = 0;
  CHECK (key (CHOICES, 13) == 0 && closed_window == CHOICES && reported[0] == 0,
         "Return saves: the same user with no new password keeps the login");
  CHECK (reloaded == 2, "(nothing new to reload)");

  click (-2, 3, Button_Adjust);
  CHECK (click (CHOICES, 7, Button_Select) == 0 && reloaded == 3, "Clear login");
  have_login = 0;   /* the stand-in for remove() */
  click (-2, 3, Button_Adjust);
  CHECK (strstr (wimp_icon_text (1, 6), "No login set"), "and it's gone");

  /* Choices: the port. */
  type (1, "12x");
  reported[0] = 0;
  click (CHOICES, 9, Button_Select);
  CHECK (strstr (reported, "from 1 to 65535"), "a port with letters in is refused");
  type (1, "0");
  reported[0] = 0;
  click (CHOICES, 9, Button_Select);
  CHECK (strstr (reported, "from 1 to 65535"), "port 0 is refused");
  type (1, "3391");
  finished = 0;
  CHECK (click (CHOICES, 9, Button_Select) == 0 && finished == 1 && started == 2
         && strcmp (started_port, "3391") == 0, "a new port restarts the server on it");
  CHECK (wimp_read_port (settings, 0) == 3391, "and is saved");
  wimp_tick ();
  CHECK (strstr (wimp_icon_text (0, 0), "port 3391") != NULL, "Status shows the new port");

  /* Choices: mute this computer while a client listens. */
  {
    const int *mi = &window_blocks[1][22 + 8 * 10];
    CHECK (window_icons[1] == 14 && (mi[4] & 0x3) == 0x3 && ((mi[4] >> 12) & 15) == 3
           && strcmp ((const char *) mi[6], "Soptoff,opton") == 0
           && strstr ((const char *) mi[5], "Mute this computer"),
           "Choices has the mute option (text and optoff/opton sprites, reports clicks)");
  }
  CHECK (o.mute_local == 0, "mute starts off (no line in the settings)");
  click (-2, 3, Button_Adjust);
  CHECK (opt_state[0] == CHOICES && opt_state[2] == 0 && opt_state[3] == 0x200000,
         "opening Choices shows it off");
  click (CHOICES, 10, Button_Select);
  CHECK (opt_state[2] == 0x200000, "a click ticks it");
  CHECK (mute_calls == 0 && o.mute_local == 0, "not used until Save");
  started = finished = 0;
  CHECK (click (CHOICES, 9, Button_Select) == 0 && mute_calls == 1 && mute_set == 1
         && o.mute_local == 1 && finished == 0 && started == 0,
         "Save: the running server mutes from now (no restart)");
  CHECK (wimp_read_mute (settings, 0) == 1 && wimp_read_port (settings, 0) == 3391,
         "and it's saved with the port");
  click (-2, 3, Button_Adjust);
  CHECK (opt_state[2] == 0x200000, "Choices shows it on");
  click (CHOICES, 10, Button_Select);
  click (CHOICES, 8, Button_Select);
  CHECK (mute_calls == 1 && o.mute_local == 1, "Cancel: unchanged");
  click (-2, 3, Button_Adjust);
  click (CHOICES, 10, Button_Select);
  CHECK (click (CHOICES, 9, Button_Select) == 0 && mute_calls == 2 && mute_set == 0
         && wimp_read_mute (settings, 1) == 0, "off again, saved");

  /* Choices: send this computer's sound to the client (off unless chosen,
     0.2-test22); muting this computer is shaded while it's off. */
  {
    const int *si = &window_blocks[1][22 + 8 * 12];
    CHECK (strstr ((const char *) si[5], "Send this computer's sound") && ((si[4] >> 12) & 15) == 3
           && strcmp ((const char *) si[6], "Soptoff,opton") == 0, "Choices has the sound option");
    CHECK (o.send_sound == 0, "sound not sent to start with");
    click (-2, 3, Button_Adjust);
    CHECK (sound_state[0] == CHOICES && sound_state[2] == 0, "shown off");
    CHECK (mute_shade[0] == CHOICES && mute_shade[2] == 0x400000, "and the mute option shaded");
    click (CHOICES, 12, Button_Select);
    CHECK (sound_state[2] == 0x200000 && mute_shade[2] == 0, "a click ticks it, and mute can be chosen");
    CHECK (sound_calls == 0 && o.send_sound == 0, "not used until Save");
    started = finished = 0;
    CHECK (click (CHOICES, 9, Button_Select) == 0 && sound_calls == 1 && sound_set == 1
           && o.send_sound == 1 && started == 0 && finished == 0,
           "Save: the running server is told (no restart)");
    CHECK (wimp_read_sound (settings, 0) == 1 && wimp_read_port (settings, 0) == 3391,
           "saved, with the port");
    click (-2, 3, Button_Adjust);
    CHECK (sound_state[2] == 0x200000 && mute_shade[2] == 0, "Choices shows it on");
    click (CHOICES, 12, Button_Select);
    CHECK (click (CHOICES, 9, Button_Select) == 0 && sound_calls == 2 && sound_set == 0
           && o.send_sound == 0 && wimp_read_sound (settings, 1) == 0, "off again, saved");
  }

  /* Choices: the detailed log (off unless chosen, 0.2-test26), told to
     the running server on Save, kept in Settings. */
  {
    const int *li = &window_blocks[1][22 + 8 * 13];
    CHECK (strstr ((const char *) li[5], "Detailed log") && ((li[4] >> 12) & 15) == 3
           && strcmp ((const char *) li[6], "Soptoff,opton") == 0, "Choices has the detailed log option");
    CHECK (o.verbose == 0, "no detailed log to start with");
    click (-2, 3, Button_Adjust);
    click (CHOICES, 13, Button_Select);
    CHECK (verbose_calls == 0 && o.verbose == 0, "a click ticks it, not used until Save");
    started = finished = 0;
    CHECK (click (CHOICES, 9, Button_Select) == 0 && verbose_calls == 1 && verbose_set == 1
           && o.verbose == 1 && started == 0 && finished == 0,
           "Save: the running server is told (no restart)");
    CHECK (wimp_read_log (settings, 0) == 1 && wimp_read_sound (settings, 1) == 0,
           "saved, with the other choices");
    click (-2, 3, Button_Adjust);
    click (CHOICES, 13, Button_Select);
    CHECK (click (CHOICES, 9, Button_Select) == 0 && verbose_calls == 2 && verbose_set == 0
           && o.verbose == 0 && wimp_read_log (settings, 1) == 0, "off again, saved");
  }

  /* Choices: only allow TLS connections. */
  {
    const int *ti = &window_blocks[1][22 + 8 * 11];
    char k[128], c[128];
    CHECK (strstr ((const char *) ti[5], "Only allow TLS") && ((ti[4] >> 12) & 15) == 3
           && strcmp ((const char *) ti[6], "Soptoff,opton") == 0, "Choices has the TLS option");
    CHECK (o.require_tls == 0, "TLS not required to start with");
    click (-2, 3, Button_Adjust);
    CHECK (tls_state[0] == CHOICES && tls_state[2] == 0, "shown off");
    click (CHOICES, 11, Button_Select);
    reported[0] = 0;
    CHECK (click (CHOICES, 9, Button_Select) == 0 && strstr (reported, "no TLS certificate")
           && tls_calls == 0 && o.require_tls == 0 && tls_state[2] == 0,
           "no certificate: TLS can't be required (it says why, the tick goes)");
    snprintf (k, sizeof k, "%s/key.pem", choices);
    snprintf (c, sizeof c, "%s/cert.pem", choices);
    f = fopen (k, "w"); fputs ("k", f); fclose (f);
    f = fopen (c, "w"); fputs ("c", f); fclose (f);
    click (CHOICES, 11, Button_Select);
    started = finished = 0;
    reported[0] = 0;
    CHECK (click (CHOICES, 9, Button_Select) == 0 && reported[0] == 0 && tls_calls == 1
           && tls_set == 1 && o.require_tls == 1 && started == 0 && finished == 0,
           "with one: Save requires TLS from the next client (no restart)");
    CHECK (wimp_read_tls (settings, 0) == 1 && wimp_read_mute (settings, 1) == 0
           && wimp_read_port (settings, 0) == 3391, "saved, with the port and mute");
    click (-2, 3, Button_Adjust);
    CHECK (tls_state[2] == 0x200000, "Choices shows it on");
    click (CHOICES, 11, Button_Select);
    CHECK (click (CHOICES, 9, Button_Select) == 0 && tls_calls == 2 && tls_set == 0
           && o.require_tls == 0 && wimp_read_tls (settings, 1) == 0, "off again, saved");
    remove (k);
    remove (c);
  }

  start_fails = 1;
  click (-2, 3, Button_Adjust);
  type (1, "80");
  reported[0] = 0;
  click (CHOICES, 9, Button_Select);
  CHECK (strstr (reported, "can't listen on port 80"), "a port that can't be used is reported");
  wimp_tick ();
  CHECK (strstr (wimp_icon_text (0, 0), "Not listening"), "and Status says so");
  start_error = "The RDPServer module isn't loaded (!RDPServer's !Run loads it).";
  reported[0] = 0;
  CHECK (click (CHOICES, 9, Button_Select) == 0 && strstr (reported, "module isn't loaded"),
         "the module's own reason is reported when it gives one");
  start_error = NULL;
  start_fails = 0;
  click (-2, 3, Button_Adjust);
  started = 0;
  CHECK (click (CHOICES, 9, Button_Select) == 0 && started == 1 && strcmp (started_port, "80") == 0,
         "Save with the same port tries it again once it's free");

  /* Keys and the rest. */
  click (-2, 3, Button_Adjust);
  closed_window = 0;
  CHECK (key (CHOICES, 0x1b) == 0 && closed_window == CHOICES, "Escape closes Choices");
  CHECK (key (STATUS, 0x181) == 0 && processed_key == 0x181, "other keys go back to the Wimp");
  b[0] = STATUS;
  CHECK (wimp_event (Close_Window_Request, b) == 0 && closed_window == STATUS, "Status closes");
  memset (b, 0, sizeof b); b[4] = 0;
  CHECK (wimp_event (User_Message, b) == 1, "Message_Quit ends the program");
  CHECK (choose (4) == 1, "and so does Quit");
  finished = 0;
  shutdowns = 0;
  wimp_close ();
  CHECK (fake_calls (Wimp_CloseDown) == 1, "Wimp_CloseDown at the end");
  CHECK (shutdowns == 1 && leaves == 0, "the server (and the module) let go of at the end");

  /* The desktop shutting down (PreQuit, flags bit 0 clear, then Quit):
     the server carries on in the module, so a client can click Restart
     (0.2-test23). TaskManager quitting just this task (bit 0 set): it
     stops as before. */
  start_fails = 0;
  windows = 0;                 /* (the fake's windows: made again) */
  CHECK (wimp_init (&w) == 0, "started again");
  memset (b, 0, sizeof b); b[0] = 24; b[4] = 8; b[5] = 1;
  CHECK (wimp_event (User_Message, b) == 0, "PreQuit for this task alone: not objected to");
  memset (b, 0, sizeof b); b[4] = 0;
  wimp_event (User_Message, b);
  shutdowns = leaves = 0;
  wimp_close ();
  CHECK (shutdowns == 1 && leaves == 0, "this task quitting alone: the server stops");
  windows = 0;
  CHECK (wimp_init (&w) == 0, "and again");
  memset (b, 0, sizeof b); b[0] = 24; b[4] = 8; b[5] = 0;
  CHECK (wimp_event (User_Message, b) == 0, "PreQuit for a shutdown: not objected to");
  memset (b, 0, sizeof b); b[4] = 0;
  CHECK (wimp_event (User_Message, b) == 1, "then Quit");
  shutdowns = leaves = 0;
  wimp_close ();
  CHECK (leaves == 1 && shutdowns == 0, "a shutdown: the server carries on without the front end");
  windows = 0;
  CHECK (wimp_init (&w) == 0, "and again");
  memset (b, 0, sizeof b); b[0] = 24; b[4] = 8; b[5] = 0;
  wimp_event (User_Message, b);
  CHECK (choose (4) == 1, "a shutdown someone objected to, then Quit from the menu");
  shutdowns = leaves = 0;
  wimp_close ();
  CHECK (shutdowns == 1 && leaves == 0, "the menu's Quit stops it, whatever came before");
  /* a short PreQuit (no flags word): a shutdown */
  windows = 0;
  CHECK (wimp_init (&w) == 0, "and again");
  memset (b, 0, sizeof b); b[0] = 20; b[4] = 8; b[5] = 1;
  wimp_event (User_Message, b);
  memset (b, 0, sizeof b); b[4] = 0;
  wimp_event (User_Message, b);
  shutdowns = leaves = 0;
  wimp_close ();
  CHECK (leaves == 1 && shutdowns == 0, "a PreQuit with no flags word is a shutdown");
  /* a shutdown cancelled (someone objected), then a bare Quit much later */
  windows = 0;
  CHECK (wimp_init (&w) == 0, "and again");
  memset (b, 0, sizeof b); b[0] = 24; b[4] = 8; b[5] = 0;
  wimp_event (User_Message, b);
  now += 3001;
  memset (b, 0, sizeof b); b[4] = 0;
  wimp_event (User_Message, b);
  shutdowns = leaves = 0;
  wimp_close ();
  CHECK (shutdowns == 1 && leaves == 0, "a Quit 30 s after a shutdown's PreQuit: not that shutdown, it stops");

  remove (settings);
  remove (log);
  rmdir (dir);
  return fake_done ("RDPServer desktop front end (fake RISC OS)");
}
