/* RDPServer: keyboard layouts for the login screen (see kbdlayout.h).

   Each layout is the national PC layout for that country, which RISC OS's
   keyboard for it follows: the number row (scan codes &02-&0D), the top
   letter row (&10-&1B), the middle row (&1E-&29, the last being the key
   left of 1), the key by Return (&2B), the bottom row (&2C-&35) and the
   key by left Shift (&56), each normal, with Shift and with AltGr. Dead
   keys (accents that wait for the next key) type nothing here. Strings
   are Latin-1; '\0' is no character.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <string.h>
#include "kbdlayout.h"

typedef struct
{
  int keyboard;
  const char *name;
  /* 12, 12, 12 (&1E-&29), 1 (&2B), 10, 1 (&56): 48 keys */
  const char *normal, *shift, *altgr;
} layout;

/* the keys, in the order of the strings */
static const unsigned char keys[48] = {
  0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
  0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B,
  0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29,
  0x2B,
  0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35,
  0x56
};

#define N "\0"
static const layout layouts[] = {
  { KBD_USA, "USA",
    "1234567890-=" "qwertyuiop[]" "asdfghjkl;'`" "\\" "zxcvbnm,./" "\\",
    "!@#$%^&*()_+" "QWERTYUIOP{}" "ASDFGHJKL:\"~" "|" "ZXCVBNM<>?" "|",
    N N N N N N N N N N N N  N N N N N N N N N N N N  N N N N N N N N N N N N  N  N N N N N N N N N N  N },
  { KBD_UK, "UK",
    "1234567890-=" "qwertyuiop[]" "asdfghjkl;'`" "#" "zxcvbnm,./" "\\",
    "!\"\xa3$%^&*()_+" "QWERTYUIOP{}" "ASDFGHJKL:@\xac" "~" "ZXCVBNM<>?" "|",
    N N N N N N N N N N N N  N N N N N N N N N N N N  N N N N N N N N N N N "\xa6"  N  N N N N N N N N N N  N },
  { KBD_GERMANY, "Germany",
    "1234567890\xdf" N "qwertzuiop\xfc+" "asdfghjkl\xf6\xe4" N "#" "yxcvbnm,.-" "<",
    "!\"\xa7$%&/()=?" N "QWERTZUIOP\xdc*" "ASDFGHJKL\xd6\xc4\xb0" "'" "YXCVBNM;:_" ">",
    N "\xb2\xb3" N N N "{[]}\\" N  "@" N N N N N N N N N N "~"  N N N N N N N N N N N N  N  N N N N N N "\xb5" N N N  "|" },
  { KBD_FRANCE, "France",
    "&\xe9\"'(-\xe8_\xe7\xe0)=" "azertyuiop" N "$" "qsdfghjklm\xf9\xb2" "*" "wxcvbn,;:!" "<",
    "1234567890\xb0+" "AZERTYUIOP" N "\xa3" "QSDFGHJKLM%" N "\xb5" "WXCVBN?./\xa7" ">",
    N N "#{[|" N "\\^@]}" N N N N N N N N N N N "\xa4"  N N N N N N N N N N N N  N  N N N N N N N N N N  N },
  { KBD_NETHERLANDS, "Netherlands",
    "1234567890/\xb0" "qwertyuiop" N "*" "asdfghjkl+" N "@" "<" "zxcvbnm,.-" "]",
    "!\"#$%&_()'?~" "QWERTYUIOP" N "|" "ASDFGHJKL\xb1" N "\xa7" ">" "ZXCVBNM;:=" "[",
    "\xb9\xb2\xb3\xbc\xbd\xbe\xa3{}" N "\\\xb8"  N N N N N N N N N N N N  N "\xdf" N N N N N N N N N "\xac"  N  N N N N N N "\xb5" N N N  "\xa6" },
};
#undef N

static const layout *
find (int keyboard)
{
  size_t i;
  for (i = 0; i < sizeof layouts / sizeof layouts[0]; i++)
    if (layouts[i].keyboard == keyboard)
      return &layouts[i];
  return NULL;
}

int
kbd_known (int keyboard)
{
  return find (keyboard) != NULL;
}

const char *
kbd_name (int keyboard)
{
  const layout *l = find (keyboard);
  return l ? l->name : "USA";
}

int
kbd_from_client (int keylayout)
{
  switch (keylayout & 0xffff)
    {
    case 0x0809: return KBD_UK;
    case 0x0409: return KBD_USA;
    case 0x0407: return KBD_GERMANY;
    case 0x040C: return KBD_FRANCE;
    case 0x0413: return KBD_NETHERLANDS;
    }
  return 0;
}

static int
letter (unsigned c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
         || (c >= 0xC0 && c <= 0xFE && c != 0xD7 && c != 0xF7 && c != 0xDF);
}

/* Upper or lower case of a Latin-1 letter. */
static unsigned
other_case (unsigned c)
{
  return letter (c) ? c ^ 0x20 : c;
}

unsigned
kbd_char (int keyboard, int code, int shift, int altgr, int caps)
{
  const layout *l = find (keyboard);
  int i;
  if (!l)
    l = find (KBD_USA);
  if (code == 0x39)
    return altgr ? 0 : ' ';
  for (i = 0; i < 48; i++)
    if (keys[i] == code)
      {
        unsigned c;
        if (altgr)
          return (unsigned char) l->altgr[i];
        c = (unsigned char) (shift ? l->shift[i] : l->normal[i]);
        /* Caps Lock: letters the other case */
        if (caps && c && letter ((unsigned char) l->normal[i]))
          c = other_case (c);
        return c;
      }
  return 0;
}

int
kbd_unicode_char (unsigned u)
{
  static const struct { unsigned short u; unsigned char c; } extra[] = {
    { 0x20AC, 0x80 }, { 0x0174, 0x81 }, { 0x0175, 0x82 }, { 0x0176, 0x85 },
    { 0x0177, 0x86 }, { 0x2026, 0x8C }, { 0x2122, 0x8D }, { 0x2030, 0x8E },
    { 0x2022, 0x8F }, { 0x2018, 0x90 }, { 0x2019, 0x91 }, { 0x2039, 0x92 },
    { 0x203A, 0x93 }, { 0x201C, 0x94 }, { 0x201D, 0x95 }, { 0x201E, 0x96 },
    { 0x2013, 0x97 }, { 0x2014, 0x98 }, { 0x2212, 0x99 }, { 0x0152, 0x9A },
    { 0x0153, 0x9B }, { 0x2020, 0x9C }, { 0x2021, 0x9D }, { 0xFB01, 0x9E },
    { 0xFB02, 0x9F }
  };
  size_t i;
  if (u == 8 || u == 9 || u == 13 || u == 27 || u == 127)
    return (int) u;
  if ((u >= 0x20 && u < 0x7F) || (u >= 0xA0 && u <= 0xFF))
    return (int) u;
  for (i = 0; i < sizeof extra / sizeof extra[0]; i++)
    if (extra[i].u == u)
      return extra[i].c;
  return -1;
}

int
kbd_unicode_bytes (unsigned u, int alphabet, unsigned char out[4])
{
  int c;
  if (alphabet == KBD_ALPHABET_UTF8)
    {
      if (u >= 0xD800 && u <= 0xDFFF)
        return 0;                       /* half a surrogate pair */
      if (u < 0x20 && u != 8 && u != 9 && u != 13 && u != 27)
        return 0;
      if (u < 0x80)
        {
          out[0] = (unsigned char) u;
          return 1;
        }
      if (u >= 0x80 && u < 0xA0)
        return 0;                       /* C1 controls */
      if (u < 0x800)
        {
          out[0] = (unsigned char) (0xC0 | (u >> 6));
          out[1] = (unsigned char) (0x80 | (u & 0x3F));
          return 2;
        }
      if (u < 0x10000)
        {
          out[0] = (unsigned char) (0xE0 | (u >> 12));
          out[1] = (unsigned char) (0x80 | ((u >> 6) & 0x3F));
          out[2] = (unsigned char) (0x80 | (u & 0x3F));
          return 3;
        }
      if (u < 0x110000)
        {
          out[0] = (unsigned char) (0xF0 | (u >> 18));
          out[1] = (unsigned char) (0x80 | ((u >> 12) & 0x3F));
          out[2] = (unsigned char) (0x80 | ((u >> 6) & 0x3F));
          out[3] = (unsigned char) (0x80 | (u & 0x3F));
          return 4;
        }
      return 0;
    }
  if (alphabet == KBD_ALPHABET_LATIN9)
    {
      /* ISO 8859-15: eight places differ from Latin-1 */
      static const struct { unsigned short u; unsigned char c; } nine[] = {
        { 0x20AC, 0xA4 }, { 0x0160, 0xA6 }, { 0x0161, 0xA8 }, { 0x017D, 0xB4 },
        { 0x017E, 0xB8 }, { 0x0152, 0xBC }, { 0x0153, 0xBD }, { 0x0178, 0xBE }
      };
      size_t i;
      for (i = 0; i < sizeof nine / sizeof nine[0]; i++)
        {
          if (nine[i].u == u)
            {
              out[0] = nine[i].c;
              return 1;
            }
          if (nine[i].c == u)
            return 0;                   /* Latin-1's character there: not in 9 */
        }
      if (u > 0xFF)
        return 0;
    }
  else if (alphabet != KBD_ALPHABET_LATIN1 && alphabet >= 100 && u >= 0x80)
    return 0;                           /* another alphabet: ASCII only */
  c = kbd_unicode_char (u);
  if (c < 0)
    return 0;
  out[0] = (unsigned char) c;
  return 1;
}
