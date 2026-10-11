#include "furigana.h"

#include <string.h>

#include "utf.h"

#define MAX_PIECES 32

/* One bracket of the ruby: the kanji run before it and its reading. */
typedef struct {
    const char *base, *reading;
    size_t base_len, reading_len;
} Piece;

/* "雨[あめ]が降[ふ]る" -> {雨, あめ}, {降, ふ}. A bracket without kanji
 * before it reads nothing in the surface and is dropped. */
static int pieces(const char *ruby, Piece *out)
{
    size_t len = strlen(ruby), i = 0, run = 0;
    int n = 0, in_run = 0;
    while (i < len && n < MAX_PIECES) {
        size_t s = i;
        uint32_t cp = vjo_utf8_next(ruby, len, &i);
        if (cp == '[') {
            const char *close = memchr(ruby + i, ']', len - i);
            if (!close)
                break;
            if (in_run) {
                out[n].base = ruby + run;
                out[n].base_len = s - run;
                out[n].reading = ruby + i;
                out[n].reading_len = (size_t)(close - (ruby + i));
                n++;
            }
            i = (size_t)(close - ruby) + 1;
            in_run = 0;
            continue;
        }
        if (vjo_is_kanji(cp)) {
            if (!in_run)
                run = s;
            in_run = 1;
        } else {
            in_run = 0;
        }
    }
    return n;
}

/* 来 read く (来る) as conjugated: き before た/て/ま/ち (来た, 来ます,
 * 来ちゃう), こ before な/ず/さ/ら/い/よ and れ except in れば (来ない,
 * 来させる, 来られる, 来い, 来よう, 来れる); else く (来る, 来れば). */
static const char *kuru(const char *next, size_t n)
{
    static const char *const ki[] = {"た", "て", "ま", "ち"};
    static const char *const ko[] = {"な", "ず", "さ", "ら", "い", "よ"};
    for (unsigned k = 0; k < sizeof(ki) / sizeof(ki[0]); k++)
        if (vjo_starts_withn(next, n, ki[k]))
            return "き";
    for (unsigned k = 0; k < sizeof(ko) / sizeof(ko[0]); k++)
        if (vjo_starts_withn(next, n, ko[k]))
            return "こ";
    if (vjo_starts_withn(next, n, "れ") && !vjo_starts_withn(next, n, "れば"))
        return "こ";
    return NULL;
}

/* 良い read いい: only the plain form is いい; 良かった, 良く, 良さ are よ. */
static const char *ii(const char *next, size_t n)
{
    return vjo_starts_withn(next, n, "い") ? NULL : "よ";
}

/* Kanji whose reading in the dictionary form changes with conjugation:
 * the reading for the kana after it, NULL = the dictionary's. */
static const struct {
    const char *base, *reading;
    const char *(*conjugated)(const char *next, size_t n);
} irregular[] = {
    {"来", "く", kuru},
    {"良", "い", ii},
};

static void put_ruby(VjoBuf *b, const char *base, size_t base_len, const char *reading, size_t reading_len)
{
    vjo_buf_putc(b, ' ');
    vjo_buf_append(b, base, base_len);
    vjo_buf_putc(b, '[');
    vjo_buf_append(b, reading, reading_len);
    vjo_buf_putc(b, ']');
}

static int piece_is(const Piece *p, const char *base, const char *reading)
{
    return p->base_len == strlen(base) && !memcmp(p->base, base, p->base_len) &&
           p->reading_len == strlen(reading) && !memcmp(p->reading, reading, p->reading_len);
}

/* p's base and reading, or the irregular reading for the kana after it. */
static void put_reading(VjoBuf *b, const Piece *p, const char *next, size_t n)
{
    for (unsigned k = 0; k < sizeof(irregular) / sizeof(irregular[0]); k++) {
        const char *r;
        if (piece_is(p, irregular[k].base, irregular[k].reading) && (r = irregular[k].conjugated(next, n))) {
            put_ruby(b, p->base, p->base_len, r, strlen(r));
            return;
        }
    }
    put_ruby(b, p->base, p->base_len, p->reading, p->reading_len);
}

char *vjo_furigana_align(VjoArena *a, const char *surface, size_t n, const char *ruby)
{
    Piece p[MAX_PIECES];
    int np = pieces(ruby ? ruby : "", p), next_piece = 0;
    size_t i = 0;
    VjoBuf b;
    vjo_buf_init(&b, a);
    while (i < n) {
        size_t s = i;
        uint32_t cp = vjo_utf8_next(surface, n, &i);
        int found = -1;
        if (!vjo_is_kanji(cp)) {
            vjo_buf_append(&b, surface + s, i - s);
            continue;
        }
        /* The piece whose kanji start here: the next one in order first,
         * then any (the ruby's order may differ from the text's). */
        for (int k = 0; k < np && found < 0; k++) {
            int q = (next_piece + k) % np;
            if (p[q].base_len <= n - s && !memcmp(surface + s, p[q].base, p[q].base_len))
                found = q;
        }
        if (found < 0) {
            vjo_buf_append(&b, surface + s, i - s);
            continue;
        }
        i = s + p[found].base_len;
        next_piece = found + 1;
        put_reading(&b, &p[found], surface + i, n - i);
    }
    if (b.oom)
        return NULL;
    return vjo_buf_cstr(&b);
}
