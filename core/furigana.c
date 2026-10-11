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

/* 良い/好い read いい: only the plain form is いい; 良かった, 良く, 良さ are よ. */
static const char *ii(const char *next, size_t n)
{
    return vjo_starts_withn(next, n, "い") ? NULL : "よ";
}

/* 行く read ゆく: its past and て forms are いった, いって. */
static const char *yuku(const char *next, size_t n)
{
    return vjo_starts_withn(next, n, "っ") ? "い" : NULL;
}

/* Kanji whose reading in the dictionary form changes with conjugation:
 * the reading for the kana after it, NULL = the dictionary's. Also applied
 * to auxiliary[]'s readings: its 来 く is kuru's key. */
static const struct {
    const char *base, *reading;
    const char *(*conjugated)(const char *next, size_t n);
} irregular[] = {
    {"来", "く", kuru},
    {"良", "い", ii},
    {"好", "い", ii},
    {"行", "ゆ", yuku},
};

/* Auxiliaries written in kanji that a token takes in with its verb
 * (来て下さい): their usual reading, for kanji the ruby has none for. after:
 * the kana right before them, AFTER_TE (て/で: て来る, て見る) or
 * AFTER_KANA (any hiragana: 待ち下さい, but not 手下さえ); next: the kana
 * that must follow, NULL = any hiragana. 来 then follows its conjugation
 * (kuru). */
enum { AFTER_TE, AFTER_KANA };
static const struct {
    const char *base, *reading, *next;
    int after;
} auxiliary[] = {
    {"下", "くだ", "さ", AFTER_KANA},    {"頂", "いただ", NULL, AFTER_KANA}, {"戴", "いただ", NULL, AFTER_KANA},
    {"仕舞", "しま", NULL, AFTER_KANA}, {"過", "す", "ぎ", AFTER_KANA},     {"行", "い", NULL, AFTER_TE},
    {"見", "み", NULL, AFTER_TE},       {"置", "お", NULL, AFTER_TE},       {"居", "い", NULL, AFTER_TE},
    {"貰", "もら", NULL, AFTER_TE},     {"来", "く", NULL, AFTER_TE},
};

static int is_hiragana(uint32_t cp)
{
    return cp >= 0x3041 && cp <= 0x309F;
}

static int piece_is(const Piece *p, const char *base, const char *reading)
{
    return p->base_len == strlen(base) && !memcmp(p->base, base, p->base_len) &&
           p->reading_len == strlen(reading) && !memcmp(p->reading, reading, p->reading_len);
}

/* Does base start the surface at s, also across line breaks between its
 * kanji (the OCR's lines)? *end: past it in the surface. */
static int base_at(const char *surface, size_t n, size_t s, const char *base, size_t base_len, size_t *end)
{
    size_t i = s, j = 0;
    while (j < base_len) {
        if (i >= n)
            return 0;
        if (j > 0 && (surface[i] == '\n' || surface[i] == '\r')) {
            i++;
            continue;
        }
        if (surface[i] != base[j])
            return 0;
        i++;
        j++;
    }
    *end = i;
    return 1;
}

/* An auxiliary at s (prev: the code point before it, 0 = none) as a piece. */
static int auxiliary_at(const char *surface, size_t n, size_t s, uint32_t prev, Piece *out)
{
    for (unsigned k = 0; k < sizeof(auxiliary) / sizeof(auxiliary[0]); k++) {
        size_t bl = strlen(auxiliary[k].base), after = s + bl, j = after;
        if (bl > n - s || memcmp(surface + s, auxiliary[k].base, bl))
            continue;
        if (auxiliary[k].after == AFTER_TE ? prev != 0x3066 /* て */ && prev != 0x3067 /* で */
                                           : !is_hiragana(prev))
            continue;
        if (auxiliary[k].next ? !vjo_starts_withn(surface + after, n - after, auxiliary[k].next)
                              : j >= n || !is_hiragana(vjo_utf8_next(surface, n, &j)))
            continue;
        out->base = auxiliary[k].base;
        out->base_len = bl;
        out->reading = auxiliary[k].reading;
        out->reading_len = strlen(auxiliary[k].reading);
        return 1;
    }
    return 0;
}

/* p's reading, or the irregular reading for the kana after it. */
static void reading_of(const Piece *p, const char *next, size_t n, VjoRuby *out)
{
    for (unsigned k = 0; k < sizeof(irregular) / sizeof(irregular[0]); k++) {
        const char *r;
        if (piece_is(p, irregular[k].base, irregular[k].reading) && (r = irregular[k].conjugated(next, n))) {
            out->reading = r;
            out->reading_len = strlen(r);
            return;
        }
    }
    out->reading = p->reading;
    out->reading_len = p->reading_len;
}

int vjo_furigana_rubies(const char *surface, size_t n, const char *ruby, VjoRuby *out, int cap)
{
    Piece p[MAX_PIECES], aux;
    int np = pieces(ruby ? ruby : "", p), next_piece = 0, count = 0;
    size_t i = 0;
    uint32_t prev = 0;
    while (i < n && count < cap) {
        size_t s = i, end = 0;
        uint32_t cp = vjo_utf8_next(surface, n, &i);
        const Piece *use = NULL;
        if (!vjo_is_kanji(cp)) {
            prev = cp;
            continue;
        }
        /* The piece whose kanji start here: the next one in order first,
         * then any (the ruby's order may differ from the text's). */
        for (int k = 0; k < np && !use; k++) {
            int q = (next_piece + k) % np;
            if (base_at(surface, n, s, p[q].base, p[q].base_len, &end)) {
                use = &p[q];
                next_piece = q + 1;
            }
        }
        if (!use && auxiliary_at(surface, n, s, prev, &aux)) {
            use = &aux;
            end = s + aux.base_len;
        }
        prev = cp;
        if (!use)
            continue;
        i = end;
        out[count].start = s;
        out[count].end = end;
        reading_of(use, surface + end, n - end, &out[count]);
        count++;
    }
    return count;
}

void vjo_furigana_render(VjoBuf *b, const char *text, size_t from, size_t to, const VjoRuby *r, int nr,
                         int space_first)
{
    size_t at = from;
    for (int k = 0; k < nr; k++) {
        if (r[k].start < from || r[k].end > to)
            continue;
        vjo_buf_append(b, text + at, r[k].start - at);
        if (b->len || space_first)
            vjo_buf_putc(b, ' ');
        for (size_t i = r[k].start; i < r[k].end; i++)
            if (text[i] != '\n' && text[i] != '\r')
                vjo_buf_putc(b, text[i]);
        vjo_buf_putc(b, '[');
        vjo_buf_append(b, r[k].reading, r[k].reading_len);
        vjo_buf_putc(b, ']');
        for (size_t i = r[k].start; i < r[k].end; i++)
            if (text[i] == '\n' || text[i] == '\r')
                vjo_buf_putc(b, text[i]);
        at = r[k].end;
    }
    vjo_buf_append(b, text + at, to - at);
}

char *vjo_furigana_align(VjoArena *a, const char *surface, size_t n, const char *ruby)
{
    int cap = (int)(n / 2) + 1; /* a kanji is 3 or 4 bytes */
    VjoRuby *r = (VjoRuby *)vjo_arena_alloc(a, sizeof(VjoRuby) * (size_t)cap);
    VjoBuf b;
    if (!r)
        return NULL;
    vjo_buf_init(&b, a);
    vjo_furigana_render(&b, surface, 0, n, r, vjo_furigana_rubies(surface, n, ruby, r, cap), 1);
    if (b.oom)
        return NULL;
    return vjo_buf_cstr(&b);
}
