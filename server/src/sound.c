/* RDPServer: sound to the client over RDPSND (see sound.h).
   PDU layouts: [MS-RDPEA] 2.2; xrdp's sesman/chansrv/sound.c (Apache-2.0)
   was the reference for what clients take.
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdint.h>
#include <string.h>

#include "arch.h"
#include "os_calls.h"
#include "parse.h"
#include "trans.h"
#include "libxrdpinc.h"
#include "ms-rdpbcgr.h"
#include "platform.h"
#include "sound.h"

#define SNDC_CLOSE       0x01
#define SNDC_WAVE        0x02
#define SNDC_WAVECONFIRM 0x05
#define SNDC_TRAINING    0x06
#define SNDC_FORMATS     0x07
#define SNDC_QUALITYMODE 0x0C

#define TSSNDCAPS_ALIVE  0x00000001

#define CHUNK 1600                /* CHANNEL_CHUNK_LENGTH */
#define BLOCK_MS 40               /* sound per Wave PDU */
#define MAX_OUTSTANDING 8         /* blocks the client hasn't confirmed */
#define MAX_BLOCK (96000 * 4 * BLOCK_MS / 1000)

static void
put16 (unsigned char *p, unsigned v)
{
  p[0] = (unsigned char) v;
  p[1] = (unsigned char) (v >> 8);
}

static void
put32 (unsigned char *p, unsigned v)
{
  put16 (p, v & 0xffff);
  put16 (p + 2, v >> 16);
}

static unsigned
get16 (const unsigned char *p)
{
  return (unsigned) p[0] | ((unsigned) p[1] << 8);
}

static unsigned
get32 (const unsigned char *p)
{
  return get16 (p) | (get16 (p + 2) << 16);
}

/* One PDU to the client, in chunks of at most CHUNK bytes. */
static int
send_pdu (rdps_sound *snd, const unsigned char *pdu, int len)
{
  int at = 0;
  while (at < len)
    {
      int n = len - at > CHUNK ? CHUNK : len - at;
      int flags = (at == 0 ? XR_CHANNEL_FLAG_FIRST : 0)
                  | (at + n == len ? XR_CHANNEL_FLAG_LAST : 0);
      if (libxrdp_send_to_channel (snd->session, snd->chan, (char *) pdu + at, n,
                                   len, flags) != 0)
        return -1;
      at += n;
    }
  return 0;
}

/* RDPSND_PDU_HEADER: msgType, bPad, BodySize */
static void
header (unsigned char *p, int type, int body)
{
  p[0] = (unsigned char) type;
  p[1] = 0;
  put16 (p + 2, (unsigned) body);
}

static unsigned
ms16 (void)
{
  return (plat_time_cs () * 10) & 0xffff;
}

void
sound_start (rdps_sound *snd, struct xrdp_session *session, int mute_local,
             int verbose, void (*say) (const char *fmt, ...))
{
  unsigned char pdu[4 + 20 + 18];
  unsigned char *p;
  unsigned rate;

  /* up again after a resize (deactivation-reactivation): the channel
     carries on as it was */
  if (snd->session == session && snd->stage > 0)
    return;
  if (snd->capturing)
    sound_stop (snd, 0);
  memset (snd, 0, sizeof *snd);
  snd->session = session;
  snd->say = say;
  snd->verbose = verbose;
  snd->mute_local = mute_local;
  snd->chan = libxrdp_get_channel_id (session, "rdpsnd");
  if (snd->chan < 0)
    return;                     /* the client didn't ask for sound */
  if (plat_sound_rate (&rate) != 0 || rate == 0)
    {
      say ("sound: no 16-bit sound system to listen to");
      return;
    }
  snd->rate = rate;
  /* Server Audio Formats and Version PDU, one format: PCM, 2 channels,
     16 bits, at the sound system's rate */
  p = pdu;
  header (p, SNDC_FORMATS, (int) sizeof pdu - 4);
  p += 4;
  put32 (p, 0);                 /* dwFlags */
  put32 (p + 4, 0);             /* dwVolume */
  put32 (p + 8, 0);             /* dwPitch */
  put16 (p + 12, 0);            /* wDGramPort */
  put16 (p + 14, 1);            /* wNumberOfFormats */
  p[16] = 0;                    /* cLastBlockConfirmed */
  put16 (p + 17, 6);            /* wVersion */
  p[19] = 0;                    /* bPad */
  p += 20;
  put16 (p, 1);                 /* WAVE_FORMAT_PCM */
  put16 (p + 2, 2);             /* nChannels */
  put32 (p + 4, rate);          /* nSamplesPerSec */
  put32 (p + 8, rate * 4);      /* nAvgBytesPerSec */
  put16 (p + 12, 4);            /* nBlockAlign */
  put16 (p + 14, 16);           /* wBitsPerSample */
  put16 (p + 16, 0);            /* cbSize */
  if (send_pdu (snd, pdu, (int) sizeof pdu) == 0)
    snd->stage = 1;
}

static void
send_training (rdps_sound *snd)
{
  unsigned char pdu[8];
  header (pdu, SNDC_TRAINING, 4);
  snd->train_sent_cs = plat_time_cs ();
  put16 (pdu + 4, ms16 ());     /* wTimeStamp */
  put16 (pdu + 6, 0);           /* wPackSize: no data follows */
  if (send_pdu (snd, pdu, (int) sizeof pdu) == 0)
    snd->stage = 2;
}

/* Client Audio Formats and Version PDU: which of its formats is ours */
static void
client_formats (rdps_sound *snd, const unsigned char *b, int len)
{
  unsigned flags, n, i;
  const unsigned char *f;
  if (len < 20)
    return;
  flags = get32 (b);
  n = get16 (b + 14);
  snd->client_version = (int) get16 (b + 17);
  f = b + 20;
  snd->client_format = -1;
  for (i = 0; i < n; i++)
    {
      unsigned cb;
      if (f + 18 > b + len)
        break;
      cb = get16 (f + 16);
      if (get16 (f) == 1 && get16 (f + 2) == 2 && get32 (f + 4) == snd->rate
          && get16 (f + 14) == 16 && snd->client_format < 0)
        snd->client_format = (int) i;
      f += 18 + cb;
    }
  if (!(flags & TSSNDCAPS_ALIVE))
    {
      /* rdesktop without -r sound, say: an ordinary choice */
      if (snd->verbose)
        snd->say ("sound: the client has no sound output");
      snd->stage = 0;
      return;
    }
  if (snd->client_format < 0)
    {
      snd->say ("sound: the client won't play %u Hz 16-bit stereo PCM", snd->rate);
      snd->stage = 0;
      return;
    }
  /* listen only now: a client without sound (rdesktop, say) costs nothing */
  {
    unsigned rate = 0;
    if (plat_sound_start (&rate) != 0)
      {
        snd->say ("sound: can't listen to this computer's sound (SharedSound)");
        snd->stage = 0;
        return;
      }
    snd->capturing = 1;
    if (rate != snd->rate)
      {
        snd->say ("sound: the sample rate changed (%u Hz, not %u)", rate, snd->rate);
        sound_stop (snd, 1);
        return;
      }
  }
  snd->say ("sound: to the client, %u Hz 16-bit stereo (its format %d, version %d)%s",
            snd->rate, snd->client_format, snd->client_version,
            snd->mute_local ? "; this computer muted meanwhile" : "");
  send_training (snd);
}

static void
client_pdu (rdps_sound *snd, const unsigned char *pdu, int len)
{
  int body;
  if (len < 4)
    return;
  body = (int) get16 (pdu + 2);
  if (body > len - 4)
    body = len - 4;
  switch (pdu[0])
    {
    case SNDC_FORMATS:
      if (snd->stage == 1)
        client_formats (snd, pdu + 4, body);
      break;
    case SNDC_TRAINING:
      if (snd->stage == 2)
        {
          snd->stage = 3;
          snd->playing_cs = plat_time_cs ();
          plat_sound_mute (snd->mute_local);
          if (snd->verbose)
            snd->say ("sound: training round trip %u cs",
                      plat_time_cs () - snd->train_sent_cs);
        }
      break;
    case SNDC_WAVECONFIRM:
      /* cConfirmedBlockNo. FreeRDP 3 confirms each block twice: only
         the first confirm of a block sent counts. */
      if (len >= 7 && (snd->pending[pdu[6] >> 3] & (1u << (pdu[6] & 7))))
        {
          snd->pending[pdu[6] >> 3] &= (unsigned char) ~(1u << (pdu[6] & 7));
          if (snd->outstanding > 0)
            snd->outstanding--;
          snd->confirmed++;
          snd->progress_cs = plat_time_cs ();
        }
      break;
    case SNDC_QUALITYMODE:
    default:
      break;
    }
}

void
sound_channel_data (rdps_sound *snd, int chan, int flags, const void *data,
                    int len, int total)
{
  if (snd->chan < 0 || chan != snd->chan || len < 0)
    return;
  if (flags & XR_CHANNEL_FLAG_FIRST)
    {
      snd->in_len = 0;
      snd->in_total = total;
      snd->in_bad = total > (int) sizeof snd->in || total < 0;
      snd->in_started = 1;
    }
  if (!snd->in_started)
    return;                     /* a piece without its start: dropped */
  if (!snd->in_bad && snd->in_len + len <= snd->in_total)
    {
      memcpy (snd->in + snd->in_len, data, (size_t) len);
      snd->in_len += len;
    }
  else
    snd->in_bad = 1;
  if (flags & XR_CHANNEL_FLAG_LAST)
    {
      if (!snd->in_bad && snd->in_len == snd->in_total)
        client_pdu (snd, snd->in, snd->in_len);
      snd->in_len = 0;
      snd->in_started = 0;
    }
}

/* What came from the sound system and what went to the client: in the
   log 5 s after the sound starts, and when it ends. */
static void
report (rdps_sound *snd, const char *when)
{
  plat_sound_info i;
  memset (&i, 0, sizeof i);
  if (snd->capturing)
    plat_sound_stats (&i);
  snd->say ("sound %s: %u blocks sent, %u confirmed, %u silent ones not sent%s",
            when, snd->sent, snd->confirmed, snd->silent_skipped,
            snd->unconfirmed_resets ? "; confirms stopped coming at times" : "");
  snd->say ("sound %s: the sound system gave %u buffers, %u with sound "
            "(%u KB, %u Hz, flags &%X)", when, i.calls, i.with_sound,
            i.written / 1024, (i.rate + 512) / 1024, i.flags);
  snd->say ("sound %s: output handler before ours &%X%s%s", when, i.before,
            i.before_ss ? " (SharedSound's)" : (i.before ? "" : " (none)"),
            i.keep ? "; SharedSound kept on" : "");
}

static int
silent (const unsigned char *b, int n)
{
  int i;
  for (i = 0; i < n; i++)
    if (b[i])
      return 0;
  return 1;
}

int
sound_step (rdps_sound *snd)
{
  static unsigned char pdu[16 + MAX_BLOCK];
  int any = 0, blocks;
  int block = (int) (snd->rate * 4 * BLOCK_MS / 1000) & ~3;
  if (snd->stage != 3 || block <= 4 || block > MAX_BLOCK)
    return 0;
  if (!snd->told && (int) (plat_time_cs () - snd->playing_cs) >= 500)
    {
      snd->told = 1;
      report (snd, "after 5 s");
    }
  /* Another program changed the sample rate: the client would play it at
     the wrong speed. Looked at once a second. */
  if ((int) (plat_time_cs () - snd->rate_checked_cs) >= 100)
    {
      unsigned rate = 0;
      snd->rate_checked_cs = plat_time_cs ();
      if (plat_sound_rate (&rate) == 0 && rate != snd->rate)
        {
          snd->say ("sound: the sample rate changed to %u Hz: no more sound to "
                    "the client this session", rate);
          sound_stop (snd, 1);
          return 0;
        }
      if (!plat_sound_ok ())
        {
          snd->say ("sound: another program took over the sound output: no more "
                    "sound to the client this session");
          sound_stop (snd, 1);
          return 0;
        }
    }
  /* Confirms stopped coming (lost, or a client that doesn't send them):
     after 2 s, carry on as if they had. */
  if (snd->outstanding >= MAX_OUTSTANDING
      && (int) (plat_time_cs () - snd->progress_cs) > 200)
    {
      memset (snd->pending, 0, sizeof snd->pending);
      snd->outstanding = 0;
      snd->unconfirmed_resets++;
    }
  for (blocks = 0; blocks < MAX_OUTSTANDING && snd->outstanding < MAX_OUTSTANDING; blocks++)
    {
      unsigned char *data = pdu + 16;
      /* at most MAX_OUTSTANDING blocks behind what's played (as much
         again as the client holds): after a longer stall, skip ahead
         rather than stay late for the rest of the session */
      int n = plat_sound_read (data, block, block * MAX_OUTSTANDING);
      if (n < block)
        break;                  /* not a whole block played yet */
      if (silent (data, n))
        {
          snd->silent_skipped++;
          continue;
        }
      /* WaveInfo PDU: header, wTimeStamp, wFormatNo, cBlockNo, bPad[3],
         Data[4] (the sound's first 4 bytes); its BodySize counts the
         whole sound. Then the Wave PDU: bPad[4], the rest of the sound. */
      snd->block_no++;
      snd->pending[snd->block_no >> 3] |= (unsigned char) (1u << (snd->block_no & 7));
      if (snd->outstanding == 0)
        snd->progress_cs = plat_time_cs ();
      header (pdu, SNDC_WAVE, 12 + n - 4);
      put16 (pdu + 4, ms16 ());
      put16 (pdu + 6, (unsigned) snd->client_format);
      pdu[8] = snd->block_no;
      pdu[9] = pdu[10] = pdu[11] = 0;
      memcpy (pdu + 12, data, 4);
      if (send_pdu (snd, pdu, 16) != 0)
        return any;
      memset (data, 0, 4);      /* the Wave PDU: bPad[4] then data[4..] */
      if (send_pdu (snd, data, n) != 0)
        return any;
      snd->outstanding++;
      snd->sent++;
      any = 1;
    }
  return any;
}

void
sound_stop (rdps_sound *snd, int send)
{
  if (snd->stage >= 1 && send)
    {
      unsigned char pdu[4];
      header (pdu, SNDC_CLOSE, 0);
      send_pdu (snd, pdu, 4);
    }
  if (snd->stage == 3 && snd->say)
    report (snd, "ended");
  if (snd->capturing)
    {
      plat_sound_mute (0);
      plat_sound_stop ();
    }
  snd->sent = snd->confirmed = snd->unconfirmed_resets = snd->silent_skipped = 0;
  snd->capturing = 0;
  snd->stage = 0;
  snd->chan = -1;
}
