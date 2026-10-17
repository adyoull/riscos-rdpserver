/* RDPServer: sound to the client over RDPSND ([MS-RDPEA], the static
   virtual channel "rdpsnd").

   The server offers PCM 16-bit stereo at half the rate the sound system
   plays at, while that's still 22 kHz or more (44.1 kHz goes as 22.05,
   48 as 24: half the bytes, 0.2-test27), and at the full rate in case
   the client won't take that (plat_sound_*); the client answers with the
   formats it takes, a training PDU goes there and back, then the sound goes as
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
  unsigned rate;            /* the sound system's rate (Hz) */
  unsigned out_rate;        /* the rate sent: rate / step */
  int step;                 /* 1, 2 or 4: frames averaged into one */
  int full_rate;            /* send at the sound system's rate (comparing) */
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
  unsigned playing_cs;      /* when training finished (stage 3) */
  int told;                 /* the 5 s report was logged */
  void (*say) (const char *fmt, ...);
  int verbose;
} rdps_sound;

/* After the client is up: if it joined rdpsnd, start the capture and
   offer the format. */
void sound_start (rdps_sound *snd, struct xrdp_session *session, int mute_local,
                  int verbose, int full_rate, void (*say) (const char *fmt, ...));

/* How many frames to average into one for a sound system at rate_hz: as
   many as keep the rate sent at 22 kHz or more (1, 2 or 4). */
int sound_step_for (unsigned rate_hz);

/* 16-bit stereo, each run of step frames averaged into one, in place;
   returns the bytes left. */
int sound_decimate (unsigned char *buf, int bytes, int step);

/* Channel data from the client (libxrdp's callback 0x5555). */
void sound_channel_data (rdps_sound *snd, int chan, int flags, const void *data,
                         int len, int total);

/* Each server step: send what has been played since. 1 if it sent
   something. */
int sound_step (rdps_sound *snd);

/* The session ends (or the server is lost: then send is 0). */
void sound_stop (rdps_sound *snd, int send);

#endif
