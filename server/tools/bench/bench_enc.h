/* candidate encoder pieces, for tools/bench */
#include <stddef.h>
int bench_tile_solid (const unsigned char *src, size_t stride, int w, int h);
void bench_convert_fast (const unsigned char *src, size_t stride, int w, int h, int cbpp, void *dst);
