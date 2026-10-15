/* RDPServer: a user name and password that clients must give.

   Kept in Choices (the file "logins"): one line per login, the name, a
   random salt and the SHA-256 of salt + password, so the password itself
   isn't stored. RDP sends the name and password inside the encrypted
   connection (the Client Info PDU), and they're checked before the client
   sees anything.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_AUTH_H
#define RDPS_AUTH_H

#define AUTH_MAX 8
#define AUTH_NAME 64

typedef struct
{
  int count;
  struct
  {
    char name[AUTH_NAME];
    unsigned char salt[16];
    unsigned char hash[32];
  } login[AUTH_MAX];
} auth_db;

/* Reads the file. A missing file is no logins (count 0). Returns 0, or -1
   if the file is there but unreadable or garbled. */
int auth_load (const char *file, auth_db *db);

/* 1 if name (any case) and password match a login. */
int auth_check (const auth_db *db, const char *name, const char *password);

/* Writes a file with just this login (a new random salt). 0 = done. */
int auth_set (const char *file, const char *name, const char *password);

/* A UTF-8 string (as RDP clients send the user name and password) to
   Latin-1 (as RISC OS types them), in place, if every character is in
   Latin-1; else it's left as it was. */
void auth_utf8_to_latin1 (char *s);

#endif
