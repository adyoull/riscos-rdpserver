/* mbedTLS configuration for RDPServer: only the crypto Standard RDP
   Security needs (RC4 is ours: mbedTLS 3 dropped it). No TLS: that is
   AcornSSL's job. No platform entropy: random numbers come from
   /dev/urandom (UnixLib's, through the CryptRandom module, on RISC OS).

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_MBEDTLS_CONFIG_H
#define RDPS_MBEDTLS_CONFIG_H

#define MBEDTLS_HAVE_ASM
#define MBEDTLS_NO_PLATFORM_ENTROPY

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_GENPRIME
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_DES_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_OID_C

#endif
