/* RDPServer: the server's RSA key (keygen.c).
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_KEYGEN_H
#define RDPS_KEYGEN_H

#define RDPS_CERT_SIGNED_LEN 300   /* 32 header + 4 exponent + 256 modulus + 8 pad */

/* Makes a 2048-bit key, signs it and writes rsakeys.ini. 0 = done. */
int rdps_keygen (const char *file);

/* Makes key.pem and a self-signed cert.pem (for TLS) from the key in
   rsakeys.ini. 0 = done. */
int rdps_make_tls_cert (const char *rsakeys, const char *key_pem, const char *cert_pem);
/* Tests only: let rdps_make_tls_cert take a key under 2048 bits. */
extern int rdps_tls_small_keys_ok;

/* Exposed for the tests. Numbers are little-endian. */
int rdps_cert_signed_part (const unsigned char e[4], const unsigned char n[256],
                           unsigned char out[RDPS_CERT_SIGNED_LEN]);
int rdps_sign_cert (const unsigned char e[4], const unsigned char n[256],
                    unsigned char sig[64]);
int rdps_check_cert_sig (const unsigned char e[4], const unsigned char n[256],
                         const unsigned char sig[64]);

#endif
