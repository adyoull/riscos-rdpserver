/* RDPServer: xrdp's ssl_calls interface (common/ssl_calls.h) without
   OpenSSL, on mbedTLS: the crypto Standard RDP Security uses (RC4, which
   mbedTLS 3 no longer has, is here), and since 0.2-test14 TLS 1.2 (below).

   The behaviour of each call (byte order, padding, what is returned)
   follows xrdp 0.10.6's OpenSSL version, which this replaces.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#include "arch.h"
#include "os_calls.h"
#include "platform.h"
#include "ssl_calls.h"

#include "mbedtls/bignum.h"
#include "mbedtls/rsa.h"
#include "mbedtls/md5.h"
#include "mbedtls/sha1.h"
#include "mbedtls/des.h"
#include "mbedtls/md.h"

int
ssl_init (void)
{
  return 0;
}

int
ssl_finish (void)
{
  return 0;
}

int
ssl_set_pre_master_secret_logfile (const char *filename)
{
  (void) filename;
  return 0;
}

/* --- RC4 ----------------------------------------------------------------- */

struct rc4_state
{
  unsigned char s[256];
  unsigned char i, j;
};

void *
ssl_rc4_info_create (void)
{
  return calloc (1, sizeof (struct rc4_state));
}

void
ssl_rc4_info_delete (void *rc4_info)
{
  free (rc4_info);
}

void
ssl_rc4_set_key (void *rc4_info, const char *key, int len)
{
  struct rc4_state *r = (struct rc4_state *) rc4_info;
  const unsigned char *k = (const unsigned char *) key;
  int n;
  unsigned char j = 0, t;

  for (n = 0; n < 256; n++)
    r->s[n] = (unsigned char) n;
  for (n = 0; n < 256; n++)
    {
      j = (unsigned char) (j + r->s[n] + k[n % len]);
      t = r->s[n];
      r->s[n] = r->s[j];
      r->s[j] = t;
    }
  r->i = r->j = 0;
}

void
ssl_rc4_crypt (void *rc4_info, char *data, int len)
{
  struct rc4_state *r = (struct rc4_state *) rc4_info;
  unsigned char *d = (unsigned char *) data;
  unsigned char i = r->i, j = r->j, t;
  int n;

  for (n = 0; n < len; n++)
    {
      i++;
      j = (unsigned char) (j + r->s[i]);
      t = r->s[i];
      r->s[i] = r->s[j];
      r->s[j] = t;
      d[n] ^= r->s[(unsigned char) (r->s[i] + r->s[j])];
    }
  r->i = i;
  r->j = j;
}

/* --- SHA-1 and MD5 --------------------------------------------------------- */

void *
ssl_sha1_info_create (void)
{
  mbedtls_sha1_context *c = malloc (sizeof *c);
  if (c)
    mbedtls_sha1_init (c);
  return c;
}

void
ssl_sha1_info_delete (void *sha1_info)
{
  if (sha1_info)
    {
      mbedtls_sha1_free (sha1_info);
      free (sha1_info);
    }
}

void
ssl_sha1_clear (void *sha1_info)
{
  mbedtls_sha1_starts (sha1_info);
}

void
ssl_sha1_transform (void *sha1_info, const char *data, int len)
{
  mbedtls_sha1_update (sha1_info, (const unsigned char *) data, (size_t) len);
}

void
ssl_sha1_complete (void *sha1_info, char *data)
{
  mbedtls_sha1_finish (sha1_info, (unsigned char *) data);
}

void *
ssl_md5_info_create (void)
{
  mbedtls_md5_context *c = malloc (sizeof *c);
  if (c)
    mbedtls_md5_init (c);
  return c;
}

void
ssl_md5_info_delete (void *md5_info)
{
  if (md5_info)
    {
      mbedtls_md5_free (md5_info);
      free (md5_info);
    }
}

void
ssl_md5_clear (void *md5_info)
{
  mbedtls_md5_starts (md5_info);
}

void
ssl_md5_transform (void *md5_info, const char *data, int len)
{
  mbedtls_md5_update (md5_info, (const unsigned char *) data, (size_t) len);
}

void
ssl_md5_complete (void *md5_info, char *data)
{
  mbedtls_md5_finish (md5_info, (unsigned char *) data);
}

/* --- Triple DES, CBC, no padding (FIPS encryption level) ------------------ */

struct des3_state
{
  mbedtls_des3_context ctx;
  unsigned char iv[8];
  int encrypt;
};

static void *
des3_create (const char *key, const char *ivec, int encrypt)
{
  struct des3_state *d = malloc (sizeof *d);
  if (!d)
    return NULL;
  mbedtls_des3_init (&d->ctx);
  if (encrypt)
    mbedtls_des3_set3key_enc (&d->ctx, (const unsigned char *) key);
  else
    mbedtls_des3_set3key_dec (&d->ctx, (const unsigned char *) key);
  memcpy (d->iv, ivec, 8);
  d->encrypt = encrypt;
  return d;
}

void *
ssl_des3_encrypt_info_create (const char *key, const char *ivec)
{
  return des3_create (key, ivec, 1);
}

void *
ssl_des3_decrypt_info_create (const char *key, const char *ivec)
{
  return des3_create (key, ivec, 0);
}

void
ssl_des3_info_delete (void *des3)
{
  struct des3_state *d = des3;
  if (d)
    {
      mbedtls_des3_free (&d->ctx);
      free (d);
    }
}

static int
des3_run (void *des3, int length, const char *in_data, char *out_data)
{
  struct des3_state *d = des3;
  if (d && length > 0 && (length & 7) == 0)
    mbedtls_des3_crypt_cbc (&d->ctx,
                            d->encrypt ? MBEDTLS_DES_ENCRYPT : MBEDTLS_DES_DECRYPT,
                            (size_t) length, d->iv,
                            (const unsigned char *) in_data,
                            (unsigned char *) out_data);
  return 0;
}

int
ssl_des3_encrypt (void *des3, int length, const char *in_data, char *out_data)
{
  return des3_run (des3, length, in_data, out_data);
}

int
ssl_des3_decrypt (void *des3, int length, const char *in_data, char *out_data)
{
  return des3_run (des3, length, in_data, out_data);
}

/* --- HMAC-MD5 (RDPServer: the auto-reconnect cookie's verifier) -------- */

int
rdps_hmac_md5 (const void *key, int key_len, const void *data, int data_len,
               unsigned char out[16])
{
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type (MBEDTLS_MD_MD5);
  if (!md)
    return -1;
  return mbedtls_md_hmac (md, key, (size_t) key_len, data, (size_t) data_len,
                          out) == 0 ? 0 : -1;
}

/* --- HMAC-SHA1 ------------------------------------------------------------ */

void *
ssl_hmac_info_create (void)
{
  mbedtls_md_context_t *c = malloc (sizeof *c);
  if (!c)
    return NULL;
  mbedtls_md_init (c);
  if (mbedtls_md_setup (c, mbedtls_md_info_from_type (MBEDTLS_MD_SHA1), 1) != 0)
    {
      free (c);
      return NULL;
    }
  return c;
}

void
ssl_hmac_info_delete (void *hmac)
{
  if (hmac)
    {
      mbedtls_md_free (hmac);
      free (hmac);
    }
}

void
ssl_hmac_sha1_init (void *hmac, const char *data, int len)
{
  mbedtls_md_hmac_starts (hmac, (const unsigned char *) data, (size_t) len);
}

void
ssl_hmac_transform (void *hmac, const char *data, int len)
{
  mbedtls_md_hmac_update (hmac, (const unsigned char *) data, (size_t) len);
}

void
ssl_hmac_complete (void *hmac, char *data, int len)
{
  unsigned char out[20];
  mbedtls_md_hmac_finish (hmac, out);
  memcpy (data, out, len < 20 ? (size_t) len : 20);
}

/* --- RSA: numbers are little-endian byte strings, as RDP sends them -------- */

/* How long the last ssl_mod_exp took (the server logs it with -v: the
   RSA step of a connection can't yield, so it holds the desktop). */
unsigned rdps_modexp_cs;

int
ssl_mod_exp (char *out, int out_len, const char *in, int in_len,
             const char *mod, int mod_len, const char *exp, int exp_len)
{
  mbedtls_mpi a, e, n, r;
  int rv = 0;
  unsigned t0 = plat_time_cs ();

  mbedtls_mpi_init (&a);
  mbedtls_mpi_init (&e);
  mbedtls_mpi_init (&n);
  mbedtls_mpi_init (&r);
  memset (out, 0, (size_t) out_len);
  if (mbedtls_mpi_read_binary_le (&a, (const unsigned char *) in, (size_t) in_len) == 0
      && mbedtls_mpi_read_binary_le (&e, (const unsigned char *) exp, (size_t) exp_len) == 0
      && mbedtls_mpi_read_binary_le (&n, (const unsigned char *) mod, (size_t) mod_len) == 0
      && mbedtls_mpi_exp_mod (&r, &a, &e, &n, NULL) == 0)
    {
      size_t sz = mbedtls_mpi_size (&r);
      if (sz <= (size_t) out_len
          && mbedtls_mpi_write_binary_le (&r, (unsigned char *) out, (size_t) out_len) == 0)
        rv = (int) sz;
    }
  mbedtls_mpi_free (&a);
  mbedtls_mpi_free (&e);
  mbedtls_mpi_free (&n);
  mbedtls_mpi_free (&r);
  rdps_modexp_cs = plat_time_cs () - t0;
  return rv;
}

/* Random bytes for key generation: g_random (/dev/urandom). */
static int
rng (void *p, unsigned char *buf, size_t len)
{
  (void) p;
  g_random ((char *) buf, (int) len);
  return 0;
}

/* returns error. exp is passed in, mod and pri are passed out, all
   little-endian and zero-padded to their lengths. */
int
ssl_gen_key_xrdp1 (int key_size_in_bits, const char *exp, int exp_len,
                   char *mod, int mod_len, char *pri, int pri_len)
{
  mbedtls_rsa_context rsa;
  mbedtls_mpi n, d;
  int e_val, error = 1;

  if ((exp_len != 4) || ((mod_len != 64) && (mod_len != 256))
      || ((pri_len != 64) && (pri_len != 256)))
    return 1;
  e_val = (unsigned char) exp[0] | ((unsigned char) exp[1] << 8)
          | ((unsigned char) exp[2] << 16) | ((unsigned char) exp[3] << 24);
  mbedtls_rsa_init (&rsa);
  mbedtls_mpi_init (&n);
  mbedtls_mpi_init (&d);
  if (mbedtls_rsa_gen_key (&rsa, rng, NULL, (unsigned) key_size_in_bits, e_val) == 0
      && mbedtls_rsa_export (&rsa, &n, NULL, NULL, &d, NULL) == 0
      && mbedtls_mpi_size (&n) <= (size_t) mod_len
      && mbedtls_mpi_size (&d) <= (size_t) pri_len
      && mbedtls_mpi_write_binary_le (&n, (unsigned char *) mod, (size_t) mod_len) == 0
      && mbedtls_mpi_write_binary_le (&d, (unsigned char *) pri, (size_t) pri_len) == 0)
    error = 0;
  mbedtls_mpi_free (&n);
  mbedtls_mpi_free (&d);
  mbedtls_rsa_free (&rsa);
  return error;
}

/* --- TLS (0.2-test14): mbedTLS's TLS 1.2 server -------------------------------------
   libxrdp starts TLS after the X.224 exchange on the connection it already
   has (trans_set_tls_mode: ssl_tls_create, then ssl_tls_accept), then
   reads and writes through ssl_tls_read and ssl_tls_write. The records go
   through our own socket calls (so through the Internet module's SWIs in
   the module), never blocking: where OpenSSL's version waits for the
   socket, these say "would block" (errno EWOULDBLOCK, as a socket does) and
   trans waits or keeps the data queued (trans->wait_s), calling again with
   the same data, as mbedTLS requires after MBEDTLS_ERR_SSL_WANT_WRITE.
   Only the handshake waits here, as it can't be left half done. */

#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "log.h"
#include "trans.h"
#include "tls.h"
#include <errno.h>

/* mbedTLS's own (library/ssl_misc.h, not a public header; it's what
   mbedtls_ssl_write itself calls to finish a record): sends what's left
   of the record in progress. 0 when all of it has gone. */
int mbedtls_ssl_flush_output (mbedtls_ssl_context *ssl);

#define NET_SEND_FAILED (-0x004E)       /* MBEDTLS_ERR_NET_SEND_FAILED */
#define NET_RECV_FAILED (-0x004C)       /* MBEDTLS_ERR_NET_RECV_FAILED */
#define HANDSHAKE_CS 3000               /* the client gets 30 s */

struct ssl_tls
{
  struct trans *trans;
  char *key, *cert;                     /* file names */
  mbedtls_ssl_context ssl;
  int pending;                          /* bytes of the caller's data in a
                                           record not all sent yet (0: none) */
};

/* The key, certificate and TLS settings: read and checked once (the key
   check is a full RSA operation), kept until the server stops or starts
   again (rdps_tls_forget), shared by each connection in turn. */
static struct
{
  int loaded;
  char key[300], cert[300];
  mbedtls_x509_crt crt;
  mbedtls_pk_context pk;
  mbedtls_ssl_config conf;
} creds;

/* the longest the last handshake held the machine in one go, for the log */
unsigned rdps_tls_handshake_cs;

static int
bio_send (void *ctx, const unsigned char *buf, size_t len)
{
  struct ssl_tls *t = ctx;
  int n = g_sck_send ((int) t->trans->sck, buf, (unsigned) len, 0);
  if (n > 0)
    return n;
  if (n == 0 || g_sck_last_error_would_block ((int) t->trans->sck))
    return MBEDTLS_ERR_SSL_WANT_WRITE;  /* (0 would read as "all sent") */
  return NET_SEND_FAILED;
}

static int
bio_recv (void *ctx, unsigned char *buf, size_t len)
{
  struct ssl_tls *t = ctx;
  int n = g_sck_recv ((int) t->trans->sck, buf, (unsigned) len, 0);
  if (n >= 0)
    return n;                           /* 0: the client closed it */
  return g_sck_last_error_would_block ((int) t->trans->sck)
         ? MBEDTLS_ERR_SSL_WANT_READ : NET_RECV_FAILED;
}

/* The whole of a (PEM) file, with a 0 after it as mbedTLS wants. */
static unsigned char *
slurp (const char *name, size_t *len)
{
  FILE *f = fopen (name, "rb");
  unsigned char *d = NULL;
  size_t got = 0, size = 0;
  if (!f)
    return NULL;
  for (;;)
    {
      size_t n;
      if (got + 1024 + 1 > size)
        {
          unsigned char *m = realloc (d, size = got + 4096 + 1);
          if (!m)
            {
              free (d);
              fclose (f);
              return NULL;
            }
          d = m;
        }
      n = fread (d + got, 1, size - got - 1, f);
      if (n == 0)
        break;
      got += n;
    }
  fclose (f);
  d[got] = 0;
  *len = got + 1;
  return d;
}

static char *
copy_str (const char *a)
{
  size_t n = strlen (a ? a : "") + 1;
  char *d = malloc (n);
  if (d)
    memcpy (d, a ? a : "", n);
  return d;
}

struct ssl_tls *
ssl_tls_create (struct trans *trans, const char *key, const char *cert)
{
  struct ssl_tls *t = calloc (1, sizeof *t);
  if (!t)
    return NULL;
  t->trans = trans;
  t->key = copy_str (key);
  t->cert = copy_str (cert);
  if (!t->key || !t->cert)
    {
      free (t->key);
      free (t->cert);
      free (t);
      return NULL;
    }
  mbedtls_ssl_init (&t->ssl);
  return t;
}

void
rdps_tls_forget (void)
{
  if (!creds.loaded)
    return;
  mbedtls_ssl_config_free (&creds.conf);
  mbedtls_x509_crt_free (&creds.crt);
  mbedtls_pk_free (&creds.pk);
  creds.loaded = 0;
}

/* Reads and checks the key and certificate, and sets TLS up: once. */
static int
load_creds (const char *key, const char *cert)
{
  unsigned char *d;
  size_t len = 0;
  int r;

  if (creds.loaded && strcmp (creds.key, key) == 0 && strcmp (creds.cert, cert) == 0)
    return 0;
  rdps_tls_forget ();
  mbedtls_x509_crt_init (&creds.crt);
  mbedtls_pk_init (&creds.pk);
  mbedtls_ssl_config_init (&creds.conf);
  creds.loaded = 1;                     /* (so rdps_tls_forget frees them) */
  snprintf (creds.key, sizeof creds.key, "%s", key);
  snprintf (creds.cert, sizeof creds.cert, "%s", cert);
  d = slurp (cert, &len);
  r = d ? mbedtls_x509_crt_parse (&creds.crt, d, len) : -1;
  free (d);
  if (r != 0)
    {
      LOG (LOG_LEVEL_ERROR, "TLS: can't read the certificate %s (-0x%04X)", cert,
           (unsigned) -r);
      rdps_tls_forget ();
      return 1;
    }
  d = slurp (key, &len);
  r = d ? mbedtls_pk_parse_key (&creds.pk, d, len, NULL, 0, rng, NULL) : -1;
  if (d)
    {
      memset (d, 0, len);
      free (d);
    }
  if (r != 0)
    {
      LOG (LOG_LEVEL_ERROR, "TLS: can't read the key %s (-0x%04X)", key, (unsigned) -r);
      rdps_tls_forget ();
      return 1;
    }
  if (mbedtls_pk_check_pair (&creds.crt.pk, &creds.pk, rng, NULL) != 0)
    {
      LOG (LOG_LEVEL_ERROR, "TLS: the key %s isn't the certificate's", key);
      rdps_tls_forget ();
      return 1;
    }
  if ((r = mbedtls_ssl_config_defaults (&creds.conf, MBEDTLS_SSL_IS_SERVER,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT)) != 0
      || (r = mbedtls_ssl_conf_own_cert (&creds.conf, &creds.crt, &creds.pk)) != 0)
    {
      LOG (LOG_LEVEL_ERROR, "TLS: setting up failed (-0x%04X)", (unsigned) -r);
      rdps_tls_forget ();
      return 1;
    }
  mbedtls_ssl_conf_rng (&creds.conf, rng, NULL);
  mbedtls_ssl_conf_min_tls_version (&creds.conf, MBEDTLS_SSL_VERSION_TLS1_2);
  mbedtls_ssl_conf_max_tls_version (&creds.conf, MBEDTLS_SSL_VERSION_TLS1_2);
  return 0;
}

int
ssl_tls_accept (struct ssl_tls *self, long ssl_protocols, const char *tls_ciphers)
{
  int r;
  unsigned start, longest, t0 = plat_time_cs ();

  (void) ssl_protocols;
  (void) tls_ciphers;
  if (load_creds (self->key, self->cert) != 0)
    return 1;
  if ((r = mbedtls_ssl_setup (&self->ssl, &creds.conf)) != 0)
    {
      LOG (LOG_LEVEL_ERROR, "TLS: setting up failed (-0x%04X)", (unsigned) -r);
      return 1;
    }
  mbedtls_ssl_set_bio (&self->ssl, self, bio_send, bio_recv, NULL);

  start = plat_time_cs ();
  longest = start - t0;                 /* reading the files the first time */
  for (;;)
    {
      unsigned t1 = plat_time_cs ();
      r = mbedtls_ssl_handshake (&self->ssl);
      if (plat_time_cs () - t1 > longest)
        longest = plat_time_cs () - t1;
      if (r == 0)
        break;
      if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          LOG (LOG_LEVEL_ERROR, "TLS: the handshake failed (-0x%04X)", (unsigned) -r);
          return 1;
        }
      if ((int) (plat_time_cs () - start) >= HANDSHAKE_CS
          || (self->trans->is_term && self->trans->is_term ()))
        {
          LOG (LOG_LEVEL_ERROR, "TLS: the client didn't finish the handshake");
          return 1;
        }
      if (r == MBEDTLS_ERR_SSL_WANT_READ)
        g_sck_can_recv ((int) self->trans->sck, 100);
      else
        g_sck_can_send ((int) self->trans->sck, 100);
    }
  rdps_tls_handshake_cs = longest;
  return 0;
}

int
ssl_tls_disconnect (struct ssl_tls *self)
{
  if (self)
    mbedtls_ssl_close_notify (&self->ssl);   /* once: never waits */
  return 0;
}

void
ssl_tls_delete (struct ssl_tls *self)
{
  if (!self)
    return;
  mbedtls_ssl_free (&self->ssl);
  free (self->key);
  free (self->cert);
  free (self);
}

int
ssl_tls_read (struct ssl_tls *tls, char *data, int length)
{
  int r = mbedtls_ssl_read (&tls->ssl, (unsigned char *) data, (size_t) length);
  if (r > 0)
    return r;
  if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
    {
      errno = EWOULDBLOCK;
      return -1;
    }
  if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
    return 0;                           /* closed */
  LOG (LOG_LEVEL_ERROR, "TLS: reading failed (-0x%04X)", (unsigned) -r);
  errno = EIO;
  return -1;
}

/* trans calls again with the same data after "would block", and mbedTLS
   wants exactly that after MBEDTLS_ERR_SSL_WANT_WRITE: the record is made,
   only sending it is left. But other output can push that record out in
   between (an alert from ssl_tls_read, say), and then mbedTLS would take
   the same data again as new and send it twice. So the record's length is
   kept (pending), and the second call only finishes it. Likewise output
   left over from elsewhere is sent before any new data goes in. */
int
ssl_tls_write (struct ssl_tls *tls, const char *data, int length)
{
  int r;
  if (tls->pending > 0)
    {
      r = mbedtls_ssl_flush_output (&tls->ssl);
      if (r == 0)
        {
          int n = tls->pending;
          tls->pending = 0;
          return n < length ? n : length;
        }
    }
  else if (tls->ssl.MBEDTLS_PRIVATE (out_left) > 0)
    r = mbedtls_ssl_flush_output (&tls->ssl);
  else
    r = 0;
  if (r == 0 && tls->pending == 0)
    {
      int fit = mbedtls_ssl_get_max_out_record_payload (&tls->ssl);
      r = mbedtls_ssl_write (&tls->ssl, (const unsigned char *) data, (size_t) length);
      if (r >= 0)
        return r;
      if (r == MBEDTLS_ERR_SSL_WANT_WRITE && tls->ssl.MBEDTLS_PRIVATE (out_left) > 0)
        tls->pending = fit > 0 && fit < length ? fit : length;
    }
  if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
    {
      errno = EWOULDBLOCK;              /* call again with the same data */
      return -1;
    }
  LOG (LOG_LEVEL_ERROR, "TLS: sending failed (-0x%04X)", (unsigned) -r);
  errno = EIO;
  return -1;
}

/* The certificate's fingerprints, as clients show them when they ask
   whether to trust it: SHA-1 (Windows' "thumbprint") and SHA-256. 0 if
   the file is a certificate. */
int
rdps_tls_fingerprints (const char *cert, char *sha1, int sha1_size,
                       char *sha256, int sha256_size)
{
  mbedtls_x509_crt crt;
  unsigned char h[32];
  size_t len = 0;
  unsigned char *d = slurp (cert, &len);
  int r = -1, i;

  mbedtls_x509_crt_init (&crt);
  if (d && mbedtls_x509_crt_parse (&crt, d, len) == 0
      && sha1_size >= 60 && sha256_size >= 96)
    {
      mbedtls_md (mbedtls_md_info_from_type (MBEDTLS_MD_SHA1), crt.raw.p, crt.raw.len, h);
      sha1[0] = 0;
      for (i = 0; i < 20; i++)
        snprintf (sha1 + strlen (sha1), 4, i ? ":%02X" : "%02X", h[i]);
      mbedtls_md (mbedtls_md_info_from_type (MBEDTLS_MD_SHA256), crt.raw.p, crt.raw.len, h);
      sha256[0] = 0;
      for (i = 0; i < 32; i++)
        snprintf (sha256 + strlen (sha256), 4, i ? ":%02X" : "%02X", h[i]);
      r = 0;
    }
  mbedtls_x509_crt_free (&crt);
  free (d);
  return r;
}

int
rdps_tls_pending (const struct ssl_tls *tls)
{
  return tls && (mbedtls_ssl_get_bytes_avail (&tls->ssl) > 0
                 || mbedtls_ssl_check_pending (&tls->ssl));
}

int
ssl_tls_can_recv (struct ssl_tls *tls, int sck, int millis)
{
  if (rdps_tls_pending (tls))
    return 1;                           /* already read from the socket */
  return g_sck_can_recv (sck, millis);
}

const char *
ssl_get_version (const struct ssl_tls *ssl)
{
  return ssl ? mbedtls_ssl_get_version (&ssl->ssl) : "none";
}

const char *
ssl_get_cipher_name (const struct ssl_tls *ssl)
{
  return ssl ? mbedtls_ssl_get_ciphersuite (&ssl->ssl) : "none";
}

int
ssl_get_protocols_from_string (const char *str, long *ssl_protocols)
{
  (void) str;
  *ssl_protocols = 0;
  return 0;
}

const char *
get_openssl_version (void)
{
  return "mbedTLS";
}

tintptr
ssl_get_rwo (const struct ssl_tls *ssl)
{
  (void) ssl;
  return 0;
}

/* --- how fast this computer encrypts (for the Log) --------------------- */
#include "mbedtls/chachapoly.h"
#include "mbedtls/gcm.h"

/* KB/s that a TLS cipher encrypts here (1: ChaCha20-Poly1305, 2:
   AES-128-GCM), timed over about cs centiseconds; 0 if it can't be. */
unsigned
rdps_tls_speed (int which, unsigned cs)
{
  static unsigned char buf[16384];
  unsigned char key[32], iv[12], tag[16];
  mbedtls_chachapoly_context cp;
  mbedtls_gcm_context g;
  unsigned t0, n = 0, el = 0;
  int ok;

  memset (key, 0x5a, sizeof key);
  memset (iv, 0xa5, sizeof iv);
  if (which == 1)
    {
      mbedtls_chachapoly_init (&cp);
      ok = mbedtls_chachapoly_setkey (&cp, key) == 0;
    }
  else
    {
      mbedtls_gcm_init (&g);
      ok = mbedtls_gcm_setkey (&g, MBEDTLS_CIPHER_ID_AES, key, 128) == 0;
    }
  if (ok)
    {
      t0 = plat_time_cs ();
      while (plat_time_cs () == t0)
        ;                       /* from the start of a tick */
      t0 = plat_time_cs ();
      do
        {
          if (which == 1)
            mbedtls_chachapoly_encrypt_and_tag (&cp, sizeof buf, iv, NULL, 0, buf, buf, tag);
          else
            mbedtls_gcm_crypt_and_tag (&g, MBEDTLS_GCM_ENCRYPT, sizeof buf, iv, sizeof iv,
                                       NULL, 0, buf, buf, sizeof tag, tag);
          n++;
          el = plat_time_cs () - t0;
        }
      while (el < cs && n < 100000);
    }
  if (which == 1)
    mbedtls_chachapoly_free (&cp);
  else
    mbedtls_gcm_free (&g);
  return ok && el ? n * (unsigned) (sizeof buf / 1024) * 100 / el : 0;
}
