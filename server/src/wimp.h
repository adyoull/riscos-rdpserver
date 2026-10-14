/* RDPServer: the desktop front end (RISC OS only).

   An icon on the icon bar (a different picture while a client is
   connected); its menu has Info, Status..., Choices..., Disconnect and
   Quit. Status shows what the server is doing and its last messages;
   Choices sets the port and the user name and password clients must give.

   The server runs from the Wimp's poll loop: Wimp_PollIdle sleeps until
   the server next needs to run (server_step says when), so it uses no
   time while nothing is happening. The windows and menu are built in
   code, with the block layouts from the PRMs (Wimp_CreateWindow,
   Wimp_CreateIcon, Wimp_CreateMenu), as in riscos-crossdev's Wimp
   example. Split like that example, so all of it can be tested against
   the Fake RISC OS: wimp_run is only the poll loop.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_WIMP_H
#define RDPS_WIMP_H

#include "server.h"

/* Wimp_Poll reason codes and mouse buttons used here. */
enum { Null_Reason = 0, Open_Window_Request = 2, Close_Window_Request = 3,
       Mouse_Click = 6, Key_Pressed = 8, Menu_Selection = 9,
       User_Message = 17, User_Message_Recorded = 18 };
enum { Button_Adjust = 1, Button_Menu = 2, Button_Select = 4 };
#define Message_Quit 0

/* What the front end needs to know. o is used for the server and kept
   (its port is changed from Choices). */
typedef struct
{
  server_options *opt;
  const char *version;        /* "0.1-test3" */
  const char *settings_file;  /* the port, from Choices */
  const char *logins_file;    /* see auth.h */
  const char *log_file;       /* the server's messages */
} wimp_setup;

/* Wimp_Initialise, the icon bar icon, the windows; then starts the
   server (a failure to listen is reported, and the program carries on so
   the port can be changed). 0 if the desktop part went well. */
int wimp_init (const wimp_setup *w);

/* Acts on one Wimp_Poll event. Returns 1 when the program should end. */
int wimp_event (int reason, int *block);

/* Runs the server and keeps the windows up to date; returns the
   monotonic time at which it next wants to run. */
unsigned wimp_tick (void);

/* Stops the server, Wimp_CloseDown. */
void wimp_close (void);

/* Shows an error in a box. */
void wimp_report (const char *text);

/* The settings file: "port N" and "mute_local 0|1" lines. The readers
   return what was read, or def. */
int wimp_read_port (const char *file, int def);
int wimp_read_mute (const char *file, int def);
int wimp_write_settings (const char *file, int port, int mute_local);

/* All of it: wimp_init, the poll loop, wimp_close. Returns the exit
   status. */
int wimp_run (const wimp_setup *w);

/* For tests: the text of icon i in the Status (0), Choices (1) or
   Info (2) window, and whether the icon bar shows the connected picture. */
const char *wimp_icon_text (int window, int icon);
int wimp_shows_connected (void);

#endif
