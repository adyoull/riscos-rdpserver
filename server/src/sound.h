/* RDPServer: sound to the client over RDPSND ([MS-RDPEA], the static
   virtual channel "rdpsnd").

   The server offers one format, PCM 16-bit stereo at the rate the sound
   system plays at (plat_sound_*), the client answers with the formats it
   takes, a training PDU goes there and back, then the sound goes as
   WaveInfo + Wave PDU pairs (version 6: every client takes them), the
   client confirming each block. Silent blocks aren't sent.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_SOUND_H
#define RDPS_SOUND_H

struct xrdp_session;

typedef struct
{
  struct xrdp_session *session;
  int chan;                 /* rdpsnd's channel id, or -1 */
  int stage;                /* 0 off, 1 formats sent, 2 training sent, 3 playing */
  int capturing;            /* plat_sound_start succeeded */
  unsigned rate;            /* the rate offered (Hz) */
  int client_format;        /* index of our format in the client's list */
  int client_version;
  unsigned char block_no;
  int outstanding;          /* blocks sent, not yet confirmed */
  unsigned char pending[32];/* which block numbers those are */
  unsigned progress_cs;     /* when the last confirm came (or sending began) */
  unsigned unconfirmed_resets;
  unsigned sent, confirmed, silent_skipped;
  unsigned train_sent_cs;
  int mute_local;
  /* reassembly of the client's PDUs */
  unsigned char in[4096];
  int in_len, in_total, in_bad, in_started;
  unsigned rate_checked_cs;
  void (*say) (const char *fmt, ...);
  int verbose;
} rdps_sound;

/* After the client is up: if it joined rdpsnd, start the capture and
   offer the format. */
void sound_start (rdps_sound *snd, struct xrdp_session *session, int mute_local,
                  int verbose, void (*say) (const char *fmt, ...));

/* Channel data from the client (libxrdp's callback 0x5555). */
void sound_channel_data (rdps_sound *snd, int chan, int flags, const void *data,
                         int len, int total);

/* Each server step: send what has been played since. 1 if it sent
   something. */
int sound_step (rdps_sound *snd);

/* The session ends (or the server is lost: then send is 0). */
void sound_stop (rdps_sound *snd, int send);

#endif
