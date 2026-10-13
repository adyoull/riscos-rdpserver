/* RDPServer: xrdp's ssl_calls interface (common/ssl_calls.h) without
   OpenSSL. The crypto Standard RDP Security uses comes from mbedTLS (its
   crypto part only); RC4, which mbedTLS 3 no longer has, is here. TLS is
   not here: it will go through the AcornSSL module, and until AcornSSL can
   act as a server the TLS calls fail, so libxrdp offers RDP security only.

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

/* --- TLS: not yet (AcornSSL server mode) ---------------------------------- */

struct ssl_tls *
ssl_tls_create (struct trans *trans, const char *key, const char *cert)
{
  (void) trans; (void) key; (void) cert;
  return NULL;
}

int
ssl_tls_accept (struct ssl_tls *self, long ssl_protocols, const char *tls_ciphers)
{
  (void) self; (void) ssl_protocols; (void) tls_ciphers;
  return 1;
}

int
ssl_tls_disconnect (struct ssl_tls *self)
{
  (void) self;
  return 0;
}

void
ssl_tls_delete (struct ssl_tls *self)
{
  (void) self;
}

int
ssl_tls_read (struct ssl_tls *tls, char *data, int length)
{
  (void) tls; (void) data; (void) length;
  return -1;
}

int
ssl_tls_write (struct ssl_tls *tls, const char *data, int length)
{
  (void) tls; (void) data; (void) length;
  return -1;
}

int
ssl_tls_can_recv (struct ssl_tls *tls, int sck, int millis)
{
  (void) tls;
  return g_sck_can_recv (sck, millis);
}

const char *
ssl_get_version (const struct ssl_tls *ssl)
{
  (void) ssl;
  return "none";
}

const char *
ssl_get_cipher_name (const struct ssl_tls *ssl)
{
  (void) ssl;
  return "none";
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
  return "none (mbedTLS crypto; TLS through AcornSSL)";
}

tintptr
ssl_get_rwo (const struct ssl_tls *ssl)
{
  (void) ssl;
  return 0;
}
