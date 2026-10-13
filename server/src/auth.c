/* RDPServer: logins. See auth.h.
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "arch.h"
#include "os_calls.h"
#include "mbedtls/sha256.h"
#include "auth.h"

static void
hash_pw (const unsigned char salt[16], const char *password, unsigned char out[32])
{
  mbedtls_sha256_context c;
  mbedtls_sha256_init (&c);
  mbedtls_sha256_starts (&c, 0);
  mbedtls_sha256_update (&c, salt, 16);
  mbedtls_sha256_update (&c, (const unsigned char *) password, strlen (password));
  mbedtls_sha256_finish (&c, out);
  mbedtls_sha256_free (&c);
}

static int
unhex (const char *s, unsigned char *out, int n)
{
  int i;
  for (i = 0; i < n; i++)
    {
      unsigned v;
      if (sscanf (s + 2 * i, "%2x", &v) != 1)
        return -1;
      out[i] = (unsigned char) v;
    }
  return s[2 * n] == 0 || isspace ((unsigned char) s[2 * n]) ? 0 : -1;
}

static void
tohex (const unsigned char *in, int n, char *out)
{
  int i;
  for (i = 0; i < n; i++)
    sprintf (out + 2 * i, "%02x", in[i]);
}

int
auth_load (const char *file, auth_db *db)
{
  FILE *f;
  char line[256];

  memset (db, 0, sizeof *db);
  f = fopen (file, "r");
  if (!f)
    return 0;
  while (fgets (line, sizeof line, f))
    {
      char name[AUTH_NAME], salt[64], hash[96];
      if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
        continue;
      if (db->count == AUTH_MAX
          || sscanf (line, "%63s %63s %95s", name, salt, hash) != 3
          || unhex (salt, db->login[db->count].salt, 16) != 0
          || unhex (hash, db->login[db->count].hash, 32) != 0)
        {
          fclose (f);
          memset (db, 0, sizeof *db);
          return -1;
        }
      strcpy (db->login[db->count].name, name);
      db->count++;
    }
  fclose (f);
  return 0;
}

int
auth_check (const auth_db *db, const char *name, const char *password)
{
  int i, ok = 0;
  for (i = 0; i < db->count; i++)
    {
      unsigned char h[32], diff = 0;
      int k;
      if (strcasecmp (db->login[i].name, name) != 0)
        continue;
      hash_pw (db->login[i].salt, password, h);
      for (k = 0; k < 32; k++)
        diff |= (unsigned char) (h[k] ^ db->login[i].hash[k]);
      ok |= diff == 0;
    }
  return ok;
}

int
auth_set (const char *file, const char *name, const char *password)
{
  unsigned char salt[16], h[32];
  char shex[33], hhex[65];
  FILE *f;
  const char *p;

  if (!name[0] || strlen (name) >= AUTH_NAME)
    return -1;
  for (p = name; *p; p++)
    if (isspace ((unsigned char) *p))
      return -1;
  g_random ((char *) salt, 16);
  hash_pw (salt, password, h);
  tohex (salt, 16, shex);
  tohex (h, 32, hhex);
  f = fopen (file, "w");
  if (!f)
    return -1;
  fprintf (f, "# RDPServer logins: name, salt, SHA-256(salt + password)\n");
  fprintf (f, "%s %s %s\n", name, shex, hhex);
  return fclose (f) != 0 ? -1 : 0;
}
