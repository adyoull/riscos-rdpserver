/* RDPServer: the pointer shape sent to the client (see pointer.h).

   Clients draw the pointer themselves, from shapes the server sends.
   Some (Windows App, FreeRDP) show their own arrow until then; others
   (rdesktop, and so RISC OS's RDPClient) show none, so the server sends
   one as soon as the client is up.

   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include <string.h>
#include "pointer.h"

/* X: black, '.': white, anything else: transparent. Top row first. */
static const char *const arrow[] = {
  "X",
  "XX",
  "X.X",
  "X..X",
  "X...X",
  "X....X",
  "X.....X",
  "X......X",
  "X.......X",
  "X........X",
  "X.........X",
  "X......XXXXX",
  "X...X..X",
  "X..XX..X",
  "X.X  X..X",
  "XX   X..X",
  "X     X..X",
  "      X..X",
  "       X..X",
  "       X..X",
  "        XX",
};

void
pointer_arrow (unsigned char xor_data[PTR_SIZE * PTR_SIZE * 3],
               unsigned char and_mask[PTR_SIZE * PTR_SIZE / 8],
               int *hot_x, int *hot_y)
{
  int y, x, rows = (int) (sizeof arrow / sizeof arrow[0]);

  memset (xor_data, 0, PTR_SIZE * PTR_SIZE * 3);
  memset (and_mask, 0xff, PTR_SIZE * PTR_SIZE / 8);
  for (y = 0; y < rows; y++)
    {
      /* bottom row first */
      int row = PTR_SIZE - 1 - y;
      for (x = 0; x < PTR_SIZE && arrow[y][x]; x++)
        {
          char c = arrow[y][x];
          if (c != 'X' && c != '.')
            continue;
          and_mask[row * (PTR_SIZE / 8) + x / 8] &= (unsigned char) ~(0x80 >> (x % 8));
          if (c == '.')
            memset (xor_data + (row * PTR_SIZE + x) * 3, 0xff, 3);
        }
    }
  *hot_x = 0;
  *hot_y = 0;
}
