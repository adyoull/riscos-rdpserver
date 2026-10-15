/* RDPServer: the server's RSA key for Standard RDP Security, made on first
   run and kept in rsakeys.ini (the file libxrdp reads).

   Standard RDP Security sends the client a "proprietary certificate": the
   server's public key signed with the Terminal Services Signing Key, a key
   Microsoft published in [MS-RDPBCGR] 5.3.3.1.1 so that clients can check
   the signature (it proves nothing about who the server is: anyone can
   sign with it). The signing scheme and the key's values are from
   [MS-RDPBCGR] 5.3.3.1.1 and 5.3.3.1.2, done as xrdp's keygen/keygen.c
   does it (xrdp: Apache-2.0, Copyright (C) Jay Sorg 2004-2014).

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arch.h"
#include "os_calls.h"
#include "ssl_calls.h"
#include "keygen.h"

/* The Terminal Services Signing Key ([MS-RDPBCGR] 5.3.3.1.1), little-endian. */
static const unsigned char tssk_e[4] = { 0x5B, 0x7B, 0x88, 0xC0 };
static const unsigned char tssk_n[64] =
{
  0x3D, 0x3A, 0x5E, 0xBD, 0x72, 0x43, 0x3E, 0xC9,
  0x4D, 0xBB, 0xC1, 0x1E, 0x4A, 0xBA, 0x5F, 0xCB,
  0x3E, 0x88, 0x20, 0x87, 0xEF, 0xF5, 0xC1, 0xE2,
  0xD7, 0xB7, 0x6B, 0x9A, 0xF2, 0x52, 0x45, 0x95,
  0xCE, 0x63, 0x65, 0x6B, 0x58, 0x3A, 0xFE, 0xEF,
  0x7C, 0xE7, 0xBF, 0xFE, 0x3D, 0xF6, 0x5C, 0x7D,
  0x6C, 0x5E, 0x06, 0x09, 0x1A, 0xF5, 0x61, 0xBB,
  0x20, 0x93, 0x09, 0x5F, 0x05, 0x6D, 0xEA, 0x87
};
static const unsigned char tssk_d[64] =
{
  0x87, 0xA7, 0x19, 0x32, 0xDA, 0x11, 0x87, 0x55,
  0x58, 0x00, 0x16, 0x16, 0x25, 0x65, 0x68, 0xF8,
  0x24, 0x3E, 0xE6, 0xFA, 0xE9, 0x67, 0x49, 0x94,
  0xCF, 0x92, 0xCC, 0x33, 0x99, 0xE8, 0x08, 0x60,
  0x17, 0x9A, 0x12, 0x9F, 0x24, 0xDD, 0xB1, 0x24,
  0x99, 0xC7, 0x3A, 0xB8, 0x0A, 0x7B, 0x0D, 0xDD,
  0x35, 0x07, 0x79, 0x17, 0x0B, 0x51, 0x9B, 0xB3,
  0xC7, 0x10, 0x01, 0x13, 0xE7, 0x3F, 0xF3, 0x5F
};

static void put32 (unsigned char *p, unsigned v)
{
  p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = v >> 24;
}

/* The signed part of the proprietary certificate ([MS-RDPBCGR]
   2.2.1.4.3.1.1 and 2.2.1.4.3.1.1.1) for a 2048-bit key: the header, the
   RSA1 public key blob (exponent, modulus, 8 bytes of padding). */
int
rdps_cert_signed_part (const unsigned char e[4], const unsigned char n[256],
                       unsigned char out[RDPS_CERT_SIGNED_LEN])
{
  memset (out, 0, RDPS_CERT_SIGNED_LEN);
  put32 (out + 0, 1);              /* dwVersion */
  put32 (out + 4, 1);              /* dwSigAlgId: RSA */
  put32 (out + 8, 1);              /* dwKeyAlgId: RSA */
  out[12] = 0x06; out[13] = 0;     /* wPublicKeyBlobType: BB_RSA_KEY_BLOB */
  out[14] = 0x1c; out[15] = 0x01;  /* wPublicKeyBlobLen: 20 + 264 = 284 */
  memcpy (out + 16, "RSA1", 4);    /* magic */
  put32 (out + 20, 256 + 8);       /* keylen: modulus + padding */
  put32 (out + 24, 2048);          /* bitlen */
  put32 (out + 28, 255);           /* datalen: bitlen / 8 - 1 */
  memcpy (out + 32, e, 4);
  memcpy (out + 36, n, 256);
  return 0;
}

/* The signature ([MS-RDPBCGR] 5.3.3.1.2): MD5 of the signed part, padded
   to 64 bytes (0x00 after the hash, then 0xff, ending 0x01 0x00), raised to
   the signing key's private exponent. */
int
rdps_sign_cert (const unsigned char e[4], const unsigned char n[256],
                unsigned char sig[64])
{
  unsigned char part[RDPS_CERT_SIGNED_LEN], padded[64];
  void *md5 = ssl_md5_info_create ();

  if (!md5)
    return 1;
  rdps_cert_signed_part (e, n, part);
  memset (padded, 0xff, sizeof padded);
  ssl_md5_clear (md5);
  ssl_md5_transform (md5, (const char *) part, RDPS_CERT_SIGNED_LEN);
  ssl_md5_complete (md5, (char *) padded);
  ssl_md5_info_delete (md5);
  padded[16] = 0;
  padded[62] = 1;
  padded[63] = 0;
  return ssl_mod_exp ((char *) sig, 64, (const char *) padded, 64,
                      (const char *) tssk_n, 64, (const char *) tssk_d, 64) == 0;
}

/* Checks a signature with the signing key's public exponent: what an RDP
   client does. Returns 0 if it matches. */
int
rdps_check_cert_sig (const unsigned char e[4], const unsigned char n[256],
                     const unsigned char sig[64])
{
  unsigned char part[RDPS_CERT_SIGNED_LEN], got[64], want[64];
  void *md5 = ssl_md5_info_create ();

  if (!md5)
    return 1;
  rdps_cert_signed_part (e, n, part);
  memset (want, 0xff, sizeof want);
  ssl_md5_clear (md5);
  ssl_md5_transform (md5, (const char *) part, RDPS_CERT_SIGNED_LEN);
  ssl_md5_complete (md5, (char *) want);
  ssl_md5_info_delete (md5);
  want[16] = 0;
  want[62] = 1;
  want[63] = 0;
  ssl_mod_exp ((char *) got, 64, (const char *) sig, 64,
               (const char *) tssk_n, 64, (const char *) tssk_e, 4);
  return memcmp (got, want, 64) != 0;
}

static void
write_line (FILE *f, const char *name, const unsigned char *data, int len)
{
  int i;
  fprintf (f, "%s=", name);
  for (i = 0; i < len; i++)
    fprintf (f, "%s0x%2.2x", i ? "," : "", data[i]);
  fputc ('\n', f);
}

int
rdps_keygen (const char *file)
{
  static const unsigned char e[4] = { 0x01, 0x00, 0x01, 0x00 };   /* 65537 */
  unsigned char n[256], d[256], sig[64];
  FILE *f;

  if (ssl_gen_key_xrdp1 (2048, (const char *) e, 4, (char *) n, 256,
                         (char *) d, 256) != 0)
    return 1;
  if (rdps_sign_cert (e, n, sig) != 0 || rdps_check_cert_sig (e, n, sig) != 0)
    return 1;
  f = fopen (file, "w");
  if (!f)
    return 1;
  fputs ("[keys]\n", f);
  write_line (f, "pub_exp", e, 4);
  write_line (f, "pub_mod", n, 256);
  write_line (f, "pub_sig", sig, 64);
  write_line (f, "pri_exp", d, 256);
  memset (d, 0, sizeof d);
  return fclose (f) != 0;
}

/* --- the TLS certificate (0.2-test14) -------------------------------------------
   TLS needs the key as a PEM file and an X.509 certificate for it. Both
   are made from the key in rsakeys.ini (so no second key, and no second
   minute's wait, on machines that already have one): the private
   exponent and modulus give the primes back (mbedtls_rsa_complete). The
   certificate is self-signed: no one vouches for it, so clients ask the
   first time; it says it's for server authentication, as Windows wants. */
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

/* For the unit tests only: allow a key under 2048 bits. */
int rdps_tls_small_keys_ok;
#include "mbedtls/rsa.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/oid.h"
#include "mbedtls/asn1.h"

static int
cert_rng (void *p, unsigned char *buf, size_t len)
{
  (void) p;
  g_random ((char *) buf, (int) len);
  return 0;
}

/* A "name=0x01,0x02,..." line of rsakeys.ini, little-endian, as big-endian. */
static int
read_hex_line (const char *file, const char *name, unsigned char *out, int size)
{
  FILE *f = fopen (file, "r");
  char line[2048];
  size_t nl = strlen (name);
  int n = -1;
  if (!f)
    return -1;
  while (fgets (line, sizeof line, f))
    if (strncmp (line, name, nl) == 0 && line[nl] == '=')
      {
        const char *p = line + nl + 1;
        unsigned char le[512];
        int i;
        n = 0;
        while (*p && *p != '\n' && *p != '\r' && n < (int) sizeof le)
          {
            unsigned v;
            if (sscanf (p, "0x%2x", &v) != 1)
              break;
            le[n++] = (unsigned char) v;
            p += 4;
            if (*p == ',')
              p++;
          }
        memset (line, 0, sizeof line);
        if (n > size)
          n = -1;
        else
          for (i = 0; i < n; i++)
            out[i] = le[n - 1 - i];
        memset (le, 0, sizeof le);
        break;
      }
  fclose (f);
  return n;
}

/* The certificate's CN: this computer's name if it has one. */
static void
host_name (char *out, size_t size)
{
  const char *h = getenv ("Inet$HostName");
  size_t i, j = 0;
#ifndef __riscos
  char buf[128];
  if (!h || !*h)
    {
      buf[0] = 0;
      if (gethostname (buf, sizeof buf - 1) == 0)
        h = buf;
    }
#endif
  if (!h || !*h)
    h = "RISC OS";
  for (i = 0; h[i] && j + 1 < size; i++)
    if ((h[i] >= 'a' && h[i] <= 'z') || (h[i] >= 'A' && h[i] <= 'Z')
        || (h[i] >= '0' && h[i] <= '9') || h[i] == '-' || h[i] == '.' || h[i] == ' ')
      out[j++] = h[i];
  out[j] = 0;
  if (!j)
    snprintf (out, size, "RISC OS");
}

static int
write_file (const char *name, const unsigned char *data)
{
  /* only the owner can read it (Linux; RISC OS ignores it) */
  int fd = open (name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  FILE *f = fd >= 0 ? fdopen (fd, "w") : NULL;
  size_t n = strlen ((const char *) data);
  if (!f)
    return 1;
  if (fwrite (data, 1, n, f) != n)
    {
      fclose (f);
      return 1;
    }
  return fclose (f) != 0;
}

int
rdps_make_tls_cert (const char *rsakeys, const char *key_pem, const char *cert_pem)
{
  unsigned char n[256], e[4], d[256], serial[16];
  static unsigned char pem[8192];
  char subject[160], host[64], from[40], to[40];
  mbedtls_pk_context pk;
  mbedtls_x509write_cert crt;
  mbedtls_asn1_sequence eku;
  time_t now = time (NULL) - 24 * 3600;      /* clocks differ */
  struct tm *t = gmtime (&now);
  int nl, el, dl, r = 1;

  nl = read_hex_line (rsakeys, "pub_mod", n, sizeof n);
  el = read_hex_line (rsakeys, "pub_exp", e, sizeof e);
  dl = read_hex_line (rsakeys, "pri_exp", d, sizeof d);
  if (nl <= 0 || el <= 0 || dl <= 0 || !t)
    return 1;
  if (nl < 256 && !rdps_tls_small_keys_ok)
    return 1;                                 /* under 2048 bits: too weak */
  if (t->tm_year + 1900 < 2026)
    {
      /* the clock isn't set (no clock chip, no NetTime yet): clients check
         the dates against their own clocks, so start from a known past */
      static struct tm known;
      memset (&known, 0, sizeof known);
      known.tm_year = 2026 - 1900;
      known.tm_mday = 1;
      t = &known;
    }
  snprintf (from, sizeof from, "%04d%02d%02d%02d%02d%02d", t->tm_year + 1900,
            t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
  snprintf (to, sizeof to, "%04d%02d%02d%02d%02d%02d", t->tm_year + 1900 + 20,
            t->tm_mon + 1, t->tm_mday > 28 ? 28 : t->tm_mday, t->tm_hour,
            t->tm_min, t->tm_sec);
  host_name (host, sizeof host);
  snprintf (subject, sizeof subject, "CN=%s,O=RDPServer", host);

  mbedtls_pk_init (&pk);
  mbedtls_x509write_crt_init (&crt);
  memset (&eku, 0, sizeof eku);
  eku.buf.tag = MBEDTLS_ASN1_OID;
  eku.buf.p = (unsigned char *) MBEDTLS_OID_SERVER_AUTH;
  eku.buf.len = MBEDTLS_OID_SIZE (MBEDTLS_OID_SERVER_AUTH);
  cert_rng (NULL, serial, sizeof serial);
  serial[0] &= 0x7f;                          /* positive */
  serial[0] |= 0x01;
  if (mbedtls_pk_setup (&pk, mbedtls_pk_info_from_type (MBEDTLS_PK_RSA)) == 0
      && mbedtls_rsa_import_raw (mbedtls_pk_rsa (pk), n, (size_t) nl, NULL, 0, NULL, 0,
                                 d, (size_t) dl, e, (size_t) el) == 0
      && mbedtls_rsa_complete (mbedtls_pk_rsa (pk)) == 0
      && mbedtls_rsa_check_privkey (mbedtls_pk_rsa (pk)) == 0
      && mbedtls_pk_write_key_pem (&pk, pem, sizeof pem) == 0
      && write_file (key_pem, pem) == 0)
    {
      memset (pem, 0, sizeof pem);
      mbedtls_x509write_crt_set_version (&crt, MBEDTLS_X509_CRT_VERSION_3);
      mbedtls_x509write_crt_set_md_alg (&crt, MBEDTLS_MD_SHA256);
      mbedtls_x509write_crt_set_subject_key (&crt, &pk);
      mbedtls_x509write_crt_set_issuer_key (&crt, &pk);
      if (mbedtls_x509write_crt_set_subject_name (&crt, subject) == 0
          && mbedtls_x509write_crt_set_issuer_name (&crt, subject) == 0
          && mbedtls_x509write_crt_set_serial_raw (&crt, serial, sizeof serial) == 0
          && mbedtls_x509write_crt_set_validity (&crt, from, to) == 0
          && mbedtls_x509write_crt_set_basic_constraints (&crt, 0, -1) == 0
          && mbedtls_x509write_crt_set_key_usage (&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE
                                                  | MBEDTLS_X509_KU_KEY_ENCIPHERMENT) == 0
          && mbedtls_x509write_crt_set_ext_key_usage (&crt, &eku) == 0
          && mbedtls_x509write_crt_set_subject_key_identifier (&crt) == 0
          && mbedtls_x509write_crt_pem (&crt, pem, sizeof pem, cert_rng, NULL) == 0
          && write_file (cert_pem, pem) == 0)
        r = 0;
    }
  if (r)
    {
      remove (key_pem);
      remove (cert_pem);
    }
  mbedtls_x509write_crt_free (&crt);
  mbedtls_pk_free (&pk);
  memset (d, 0, sizeof d);
  memset (pem, 0, sizeof pem);
  return r;
}
