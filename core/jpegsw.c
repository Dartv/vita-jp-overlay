/* Baseline JPEG encoder (YCbCr 4:2:0, standard Huffman tables, AAN float
 * DCT): the encoder for the captured screen region sent to Lens. */
#include "jpegsw.h"

#include <string.h>

static const uint8_t zigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

static const uint8_t std_lum_q[64] = {
    16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};

static const uint8_t std_chr_q[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

/* Annex K Huffman tables: bit counts (16) then values. */
static const uint8_t dc_lum_bits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t dc_lum_val[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t dc_chr_bits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const uint8_t dc_chr_val[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t ac_lum_bits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
static const uint8_t ac_lum_val[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61,
    0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52,
    0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25,
    0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
    0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
    0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3,
    0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8,
    0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
static const uint8_t ac_chr_bits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
static const uint8_t ac_chr_val[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61,
    0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33,
    0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18,
    0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
    0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63,
    0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
    0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
    0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

typedef struct {
    uint16_t code[256];
    uint8_t len[256];
} Huff;

typedef struct {
    VjoBuf *out;
    uint32_t bitbuf;
    int bitcnt;
    Huff dc[2], ac[2];
    float fdq[2][64]; /* 1 / (quant * AAN scale), natural order */
    uint8_t q[2][64]; /* quant tables, natural order */
} Enc;

static void build_huff(Huff *h, const uint8_t bits[16], const uint8_t *vals)
{
    uint16_t code = 0;
    int k = 0;
    memset(h, 0, sizeof(*h));
    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < bits[l - 1]; i++) {
            h->code[vals[k]] = code++;
            h->len[vals[k]] = (uint8_t)l;
            k++;
        }
        code <<= 1;
    }
}

static void put_bits(Enc *e, uint32_t bits, int n)
{
    e->bitbuf = (e->bitbuf << n) | (bits & ((1u << n) - 1));
    e->bitcnt += n;
    while (e->bitcnt >= 8) {
        uint8_t c = (uint8_t)(e->bitbuf >> (e->bitcnt - 8));
        vjo_buf_putc(e->out, (char)c);
        if (c == 0xFF)
            vjo_buf_putc(e->out, 0);
        e->bitcnt -= 8;
    }
}

static void flush_bits(Enc *e)
{
    if (e->bitcnt > 0)
        put_bits(e, 0x7F, 8 - e->bitcnt); /* pad with 1s */
}

static void put_u16(VjoBuf *b, unsigned v)
{
    vjo_buf_putc(b, (char)(v >> 8));
    vjo_buf_putc(b, (char)v);
}

static void fdct(float *d)
{
    /* AAN forward DCT on rows then columns (jo_jpeg style). */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < 8; i++) {
            float *p = pass == 0 ? d + i * 8 : d + i;
            int s = pass == 0 ? 1 : 8;
            float d0 = p[0], d1 = p[s], d2 = p[2 * s], d3 = p[3 * s], d4 = p[4 * s], d5 = p[5 * s],
                  d6 = p[6 * s], d7 = p[7 * s];
            float t0 = d0 + d7, t7 = d0 - d7, t1 = d1 + d6, t6 = d1 - d6;
            float t2 = d2 + d5, t5 = d2 - d5, t3 = d3 + d4, t4 = d3 - d4;
            float t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
            float z1, z2, z3, z4, z5, z11, z13;
            p[0] = t10 + t11;
            p[4 * s] = t10 - t11;
            z1 = (t12 + t13) * 0.707106781f;
            p[2 * s] = t13 + z1;
            p[6 * s] = t13 - z1;
            t10 = t4 + t5;
            t11 = t5 + t6;
            t12 = t6 + t7;
            z5 = (t10 - t12) * 0.382683433f;
            z2 = t10 * 0.541196100f + z5;
            z4 = t12 * 1.306562965f + z5;
            z3 = t11 * 0.707106781f;
            z11 = t7 + z3;
            z13 = t7 - z3;
            p[5 * s] = z13 + z2;
            p[3 * s] = z13 - z2;
            p[s] = z11 + z4;
            p[7 * s] = z11 - z4;
        }
    }
}

static int encode_block(Enc *e, float *blk, int t, int dc_prev)
{
    int q[64], dc, diff, run = 0, last = 0;
    fdct(blk);
    for (int i = 0; i < 64; i++) {
        float v = blk[zigzag[i]] * e->fdq[t][zigzag[i]];
        q[i] = (int)(v < 0 ? v - 0.5f : v + 0.5f);
    }
    dc = q[0];
    diff = dc - dc_prev;
    {
        int a = diff < 0 ? -diff : diff, n = 0;
        while (a) {
            n++;
            a >>= 1;
        }
        put_bits(e, e->dc[t].code[n], e->dc[t].len[n]);
        if (n)
            put_bits(e, (uint32_t)(diff < 0 ? diff - 1 : diff), n);
    }
    for (int i = 63; i > 0; i--)
        if (q[i]) {
            last = i;
            break;
        }
    for (int i = 1; i <= last; i++) {
        int v = q[i];
        if (!v) {
            run++;
            continue;
        }
        while (run > 15) {
            put_bits(e, e->ac[t].code[0xF0], e->ac[t].len[0xF0]);
            run -= 16;
        }
        {
            int a = v < 0 ? -v : v, n = 0, sym;
            while (a) {
                n++;
                a >>= 1;
            }
            sym = (run << 4) | n;
            put_bits(e, e->ac[t].code[sym], e->ac[t].len[sym]);
            put_bits(e, (uint32_t)(v < 0 ? v - 1 : v), n);
        }
        run = 0;
    }
    if (last < 63)
        put_bits(e, e->ac[t].code[0], e->ac[t].len[0]);
    return dc;
}

static void write_headers(Enc *e, uint32_t w, uint32_t h)
{
    VjoBuf *b = e->out;
    static const uint8_t jfif[] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
    vjo_buf_append(b, jfif, sizeof(jfif));
    for (int t = 0; t < 2; t++) {
        vjo_buf_putc(b, (char)0xFF);
        vjo_buf_putc(b, (char)0xDB);
        put_u16(b, 67);
        vjo_buf_putc(b, (char)t);
        for (int i = 0; i < 64; i++)
            vjo_buf_putc(b, (char)e->q[t][zigzag[i]]);
    }
    {
        uint8_t sof[] = {0xFF, 0xC0, 0, 17, 8, (uint8_t)(h >> 8), (uint8_t)h, (uint8_t)(w >> 8), (uint8_t)w, 3,
                         1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1};
        vjo_buf_append(b, sof, sizeof(sof));
    }
    {
        const uint8_t *bits[4] = {dc_lum_bits, ac_lum_bits, dc_chr_bits, ac_chr_bits};
        const uint8_t *vals[4] = {dc_lum_val, ac_lum_val, dc_chr_val, ac_chr_val};
        const uint8_t cls[4] = {0x00, 0x10, 0x01, 0x11};
        for (int k = 0; k < 4; k++) {
            int n = 0;
            for (int i = 0; i < 16; i++)
                n += bits[k][i];
            vjo_buf_putc(b, (char)0xFF);
            vjo_buf_putc(b, (char)0xC4);
            put_u16(b, (unsigned)(2 + 1 + 16 + n));
            vjo_buf_putc(b, (char)cls[k]);
            vjo_buf_append(b, bits[k], 16);
            vjo_buf_append(b, vals[k], (size_t)n);
        }
    }
    {
        static const uint8_t sos[] = {0xFF, 0xDA, 0, 12, 3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0};
        vjo_buf_append(b, sos, sizeof(sos));
    }
}

static void init_tables(Enc *e, int quality)
{
    static const float aan[8] = {1.0f, 1.387039845f, 1.306562965f, 1.175875602f,
                                 1.0f, 0.785694958f, 0.541196100f, 0.275899379f};
    int scale;
    if (quality < 1)
        quality = 1;
    if (quality > 100)
        quality = 100;
    scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    for (int i = 0; i < 64; i++) {
        int l = (std_lum_q[i] * scale + 50) / 100, c = (std_chr_q[i] * scale + 50) / 100;
        e->q[0][i] = (uint8_t)(l < 1 ? 1 : l > 255 ? 255 : l);
        e->q[1][i] = (uint8_t)(c < 1 ? 1 : c > 255 ? 255 : c);
    }
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            float s = aan[r] * aan[c] * 8.0f;
            e->fdq[0][r * 8 + c] = 1.0f / (e->q[0][r * 8 + c] * s);
            e->fdq[1][r * 8 + c] = 1.0f / (e->q[1][r * 8 + c] * s);
        }
    build_huff(&e->dc[0], dc_lum_bits, dc_lum_val);
    build_huff(&e->ac[0], ac_lum_bits, ac_lum_val);
    build_huff(&e->dc[1], dc_chr_bits, dc_chr_val);
    build_huff(&e->ac[1], ac_chr_bits, ac_chr_val);
}

int vjo_jpeg_encode(VjoArena *a, uint32_t w, uint32_t h, uint32_t stride, VjoRowsFn rows, void *ud,
                    int quality, VjoBuf *out)
{
    Enc *e;
    uint8_t *band;
    int dcy = 0, dcb = 0, dcr = 0;

    if (!w || !h || w > 4096 || h > 4096 || stride < w * 4)
        return -1;
    e = (Enc *)vjo_arena_alloc(a, sizeof(Enc));
    band = (uint8_t *)vjo_arena_alloc(a, (size_t)stride * 16);
    if (!e || !band)
        return -1;
    memset(e, 0, sizeof(*e));
    init_tables(e, quality);
    e->out = out;
    write_headers(e, w, h);

    for (uint32_t y0 = 0; y0 < h; y0 += 16) {
        uint32_t n = h - y0 < 16 ? h - y0 : 16;
        if (rows(ud, y0, n, band) != (int)n)
            return -1;
        for (uint32_t r = n; r < 16; r++) /* replicate the last row */
            memcpy(band + r * stride, band + (n - 1) * stride, stride);
        for (uint32_t x0 = 0; x0 < w; x0 += 16) {
            float Y[4][64], Cb[64], Cr[64];
            memset(Cb, 0, sizeof(Cb));
            memset(Cr, 0, sizeof(Cr));
            for (int yy = 0; yy < 16; yy++) {
                for (int xx = 0; xx < 16; xx++) {
                    uint32_t x = x0 + (uint32_t)xx < w ? x0 + (uint32_t)xx : w - 1;
                    const uint8_t *p = band + (uint32_t)yy * stride + x * 4;
                    float R = p[0], G = p[1], B = p[2];
                    int blk = (yy >> 3) * 2 + (xx >> 3);
                    int idx = (yy & 7) * 8 + (xx & 7), cidx = (yy >> 1) * 8 + (xx >> 1);
                    Y[blk][idx] = 0.299f * R + 0.587f * G + 0.114f * B - 128.0f;
                    Cb[cidx] += (-0.168736f * R - 0.331264f * G + 0.5f * B) * 0.25f;
                    Cr[cidx] += (0.5f * R - 0.418688f * G - 0.081312f * B) * 0.25f;
                }
            }
            for (int k = 0; k < 4; k++)
                dcy = encode_block(e, Y[k], 0, dcy);
            dcb = encode_block(e, Cb, 1, dcb);
            dcr = encode_block(e, Cr, 1, dcr);
        }
        if (out->oom)
            return -1;
    }
    flush_bits(e);
    vjo_buf_putc(out, (char)0xFF);
    vjo_buf_putc(out, (char)0xD9);
    return out->oom ? -1 : 0;
}
