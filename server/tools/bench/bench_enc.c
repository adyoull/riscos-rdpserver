#include <stdint.h>
#include <string.h>
#include "bench_enc.h"
#include "pixels.h"
int bench_tile_solid (const unsigned char *src, size_t stride, int w, int h)
{
  uint32_t first;
  int x, y;
  memcpy (&first, src, 4);
  for (y = 0; y < h; y++)
    {
      const uint32_t *p = (const uint32_t *) (const void *) (src + y * stride);
      for (x = 0; x < w; x++)
        if (p[x] != first)
          return 0;
    }
  return 1;
}
void bench_convert_fast (const unsigned char *src, size_t stride, int w, int h, int cbpp, void *dst)
{
  pix_convert (PIX_32_TBGR, src, stride, w, h, cbpp, dst);
}
int co_inside (void) { return 0; }
