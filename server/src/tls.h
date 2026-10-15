/* RDPServer: what the server needs of the TLS layer (ssl_calls.c) beyond
   xrdp's ssl_calls.h.
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_TLS_H
#define RDPS_TLS_H

struct ssl_tls;

/* 1 if TLS has read data from the socket that hasn't been taken yet: the
   socket shows nothing, but there's more to read. */
int rdps_tls_pending (const struct ssl_tls *tls);

/* Forget the key and certificate read for TLS (the server stops, or starts
   again with new files). Never while a connection uses TLS. */
void rdps_tls_forget (void);

/* The certificate's SHA-1 and SHA-256 fingerprints, "AB:CD:...": what
   clients show when they ask whether to trust it. 0 = done. */
int rdps_tls_fingerprints (const char *cert, char *sha1, int sha1_size,
                           char *sha256, int sha256_size);

/* The longest the last TLS handshake held the machine in one go (cs). */
extern unsigned rdps_tls_handshake_cs;

/* KB/s a TLS cipher encrypts here (1: ChaCha20-Poly1305, 2: AES-128-GCM),
   timed over about cs centiseconds. */
unsigned rdps_tls_speed (int which, unsigned cs);

#endif
