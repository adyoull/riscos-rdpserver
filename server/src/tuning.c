/* RDPServer: the system variables for trying other settings (tuning.h).

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include "tuning.h"
#include "platform.h"

static const char *const names[TUNE_COUNT] = {
  [TUNE_SEND_BUFFER] = "RDPServer$SendBuffer",
  [TUNE_SCAN_ROWS] = "RDPServer$ScanRows",
  [TUNE_SAMPLE_ROWS] = "RDPServer$SampleRows",
  [TUNE_SCREEN_COPY] = "RDPServer$ScreenCopy",
  [TUNE_NEON] = "RDPServer$NEON",
  [TUNE_FILLS] = "RDPServer$Fills",
  [TUNE_BULK_COMPRESS] = "RDPServer$BulkCompress",
  [TUNE_CLICK_TIME] = "RDPServer$ClickTime",
  [TUNE_CLICKS] = "RDPServer$Clicks",
  [TUNE_SOUND_RATE] = "RDPServer$SoundRate",
};

int
tune (tune_id id)
{
  return (unsigned) id < TUNE_COUNT ? plat_var_int (names[id], -1) : -1;
}

const char *
tune_name (tune_id id)
{
  return (unsigned) id < TUNE_COUNT ? names[id] : "";
}
