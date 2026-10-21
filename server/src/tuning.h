/* RDPServer: the system variables for trying other settings
   (*Set RDPServer$... before starting; !Help's "Trying other settings").
   All of them in one table (tuning.c), read through tune().

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_TUNING_H
#define RDPS_TUNING_H

typedef enum
{
  TUNE_SEND_BUFFER,     /* RDPServer$SendBuffer: KB, 16 to 1024 */
  TUNE_SCAN_ROWS,       /* RDPServer$ScanRows: the most rows read a turn */
  TUNE_SAMPLE_ROWS,     /* RDPServer$SampleRows: 1 row in this many a pass */
  TUNE_SCREEN_COPY,     /* RDPServer$ScreenCopy: 1 words, 2 LDM, 3 NEON */
  TUNE_NEON,            /* RDPServer$NEON: 0 not to use NEON */
  TUNE_FILLS,           /* RDPServer$Fills: 0 areas of one colour as bitmaps */
  TUNE_BULK_COMPRESS,   /* RDPServer$BulkCompress: 0 no MPPC */
  TUNE_CLICK_TIME,      /* RDPServer$ClickTime: cs a click holds the button */
  TUNE_CLICKS,          /* RDPServer$Clicks: 0 buttons as the client sends them */
  TUNE_SOUND_RATE,      /* RDPServer$SoundRate: 0 the full rate */
  TUNE_COUNT
} tune_id;

/* The variable's value as a number, or -1 if it isn't set. */
int tune (tune_id id);
/* Its name, for messages. */
const char *tune_name (tune_id id);

#endif
