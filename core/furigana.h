/* Furigana: the readings of a word's kanji, as ranges of the word with a
 * reading each (the subtitles strip draws them over the text), or in Anki's
 * syntax: a space, the base, its reading in brackets (" 降[ふ]っていた"), as
 * Anki's furigana filter reads them. */
#ifndef VJO_FURIGANA_H
#define VJO_FURIGANA_H

#include <stddef.h>

#include "arena.h"

/* One kanji run's reading. */
typedef struct {
    size_t start, end;    /* bytes of the text: the kanji, and a line break between them */
    const char *reading;  /* into the ruby, or static */
    size_t reading_len;
} VjoRuby;

/* The readings of the kanji of the n bytes of surface (a word as written:
 * "来た") from ruby markup: the token's own (jpdb: "来[き]た") or its
 * dictionary form's (jiten: "来[く]る"). Each kanji of the surface takes the
 * reading of the same kanji in the ruby, also across a line break of the
 * surface; a dictionary-form reading that changes with conjugation (来 く ->
 * き/こ, 良/好 い -> よ, 行 ゆ -> い) follows the kana after it; an auxiliary
 * the ruby has no reading for takes its usual one (下さい, て来る, て見る);
 * other kanji are left without one. Writes up to cap, in text order;
 * returns the number written. */
int vjo_furigana_rubies(const char *surface, size_t n, const char *ruby, VjoRuby *out, int cap);

/* Appends text[from, to) with the readings r (sorted; those inside the
 * range) in Anki's syntax: a space before each base, but none opening an
 * empty buffer unless space_first; a line break inside a base goes after
 * it (" 今日[きょう]\n"). */
void vjo_furigana_render(VjoBuf *b, const char *text, size_t from, size_t to, const VjoRuby *r, int nr,
                         int space_first);

/* The surface with its readings (vjo_furigana_rubies) in Anki's syntax,
 * opening with a space before a base. NULL on OOM. */
char *vjo_furigana_align(VjoArena *a, const char *surface, size_t n, const char *ruby);

#endif
