/* vjo-jpegsw IN.rgba WIDTH HEIGHT OUT.jpg [QUALITY]: software JPEG encoder test. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpegsw.h"

typedef struct {
    const uint8_t *px;
    uint32_t stride;
} Src;

static int rows(void *ud, uint32_t row, uint32_t n, uint8_t *dst)
{
    Src *s = ud;
    memcpy(dst, s->px + (size_t)row * s->stride, (size_t)n * s->stride);
    return (int)n;
}

int main(int argc, char **argv)
{
    static uint8_t mem[4u << 20];
    VjoArena a;
    VjoBuf out;
    Src s;
    FILE *f;
    long n;
    uint8_t *px;
    uint32_t w, h;
    if (argc < 5) {
        fprintf(stderr, "usage: vjo-jpegsw IN.rgba W H OUT.jpg [QUALITY]\n");
        return 2;
    }
    w = (uint32_t)atoi(argv[2]);
    h = (uint32_t)atoi(argv[3]);
    f = fopen(argv[1], "rb");
    if (!f)
        return 1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    px = malloc((size_t)n);
    if (fread(px, 1, (size_t)n, f) != (size_t)n || (uint32_t)n < w * h * 4)
        return 1;
    fclose(f);
    s.px = px;
    s.stride = w * 4;
    vjo_arena_init(&a, mem, sizeof(mem));
    vjo_buf_init(&out, &a);
    if (vjo_jpeg_encode(&a, w, h, w * 4, rows, &s, argc > 5 ? atoi(argv[5]) : 90, &out) < 0) {
        fprintf(stderr, "encode failed\n");
        return 1;
    }
    f = fopen(argv[4], "wb");
    fwrite(out.data, 1, out.len, f);
    fclose(f);
    fprintf(stderr, "%u x %u -> %zu bytes (arena peak %zu)\n", w, h, out.len, a.peak);
    return 0;
}
