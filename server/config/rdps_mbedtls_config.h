/* mbedTLS configuration for RDPServer.

   - The crypto Standard RDP Security needs (RC4 is ours: mbedTLS 3
     dropped it).
   - Since 0.3: a TLS 1.2 server (RDP's "Enhanced RDP Security": libxrdp's
     ssl_tls_* calls, src/ssl_calls.c), and writing the server's
     self-signed X.509 certificate (src/keygen.c). ECDHE-RSA only (forward
     secrecy; every TLS 1.2 RDP client offers it), with AES-GCM,
     ChaCha20-Poly1305 or AES-CBC (Windows 7).
     TLS 1.3 isn't enabled (it needs mbedTLS's PSA crypto layer).
   - No platform entropy, no file or network layer: random numbers come
     from g_random (/dev/urandom; UnixLib's, through the CryptRandom
     module, on RISC OS), files are read by our own code, and the TLS
     records go through our own socket calls.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_MBEDTLS_CONFIG_H
#define RDPS_MBEDTLS_CONFIG_H

#define MBEDTLS_HAVE_ASM
#define MBEDTLS_NO_PLATFORM_ENTROPY

/* numbers, hashes, RSA */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_GENPRIME
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_DES_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_OID_C

/* ciphers for TLS */
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C

/* elliptic curves, for ECDHE */
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM

/* keys and certificates */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_PEM_WRITE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_CRT_WRITE_C

/* the TLS 1.2 server */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET

/* x509_crt.c's own inet_pton (the module has none; only used to check a
   certificate's IP address, which a server never does) */
#define MBEDTLS_TEST_SW_INET_PTON

#endif
