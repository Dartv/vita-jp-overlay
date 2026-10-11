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

static int starts_with(const char *s, size_t n, const char *prefix)
{
    size_t k = strlen(prefix);
    return n >= k && !memcmp(s, prefix, k);
}

/* 来 read く (来る) as conjugated: き before た/て/ま/ち (来た, 来ます,
 * 来ちゃう), こ before な/ず/さ/ら/い/よ and れ except in れば (来ない,
 * 来させる, 来られる, 来い, 来よう, 来れる); else く (来る, 来れば). */
static const char *kuru(const char *next, size_t n)
{
    static const char *const ki[] = {"た", "て", "ま", "ち"};
    static const char *const ko[] = {"な", "ず", "さ", "ら", "い", "よ"};
    for (unsigned k = 0; k < sizeof(ki) / sizeof(ki[0]); k++)
        if (starts_with(next, n, ki[k]))
            return "き";
    for (unsigned k = 0; k < sizeof(ko) / sizeof(ko[0]); k++)
        if (starts_with(next, n, ko[k]))
            return "こ";
    if (starts_with(next, n, "れ") && !starts_with(next, n, "れば"))
        return "こ";
    return NULL;
}

static void put_ruby(VjoBuf *b, const char *base, size_t base_len, const char *reading, size_t reading_len)
{
    vjo_buf_putc(b, ' ');
    vjo_buf_append(b, base, base_len);
    vjo_buf_putc(b, '[');
    vjo_buf_append(b, reading, reading_len);
    vjo_buf_putc(b, ']');
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
        if (p[found].reading_len == strlen("く") && !memcmp(p[found].reading, "く", p[found].reading_len) &&
            p[found].base_len == strlen("来") && !memcmp(p[found].base, "来", p[found].base_len)) {
            const char *r = kuru(surface + i, n - i);
            if (r) {
                put_ruby(&b, p[found].base, p[found].base_len, r, strlen(r));
                continue;
            }
        }
        put_ruby(&b, p[found].base, p[found].base_len, p[found].reading, p[found].reading_len);
    }
    if (b.oom)
        return NULL;
    return vjo_buf_cstr(&b);
}
