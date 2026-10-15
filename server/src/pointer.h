/* RDPServer: the pointer shape sent to the client.
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_POINTER_H
#define RDPS_POINTER_H

#define PTR_SIZE 32

/* An arrow (black, white inside), hot spot at its tip, in the form
   [MS-RDPBCGR] TS_COLORPOINTERATTRIBUTE takes: 24 bpp XOR data and a
   1 bpp AND mask (1: transparent), both bottom row first. */
void pointer_arrow (unsigned char xor_data[PTR_SIZE * PTR_SIZE * 3],
                    unsigned char and_mask[PTR_SIZE * PTR_SIZE / 8],
                    int *hot_x, int *hot_y);

#endif
