/* Furigana in Anki's syntax: a space, the base, its reading in brackets
 * (" 降[ふ]っていた"), as Anki's furigana filter reads them. */
#ifndef VJO_FURIGANA_H
#define VJO_FURIGANA_H

#include <stddef.h>

#include "arena.h"

/* The n bytes of surface (a word as written: "来た") with readings taken
 * from ruby markup: the token's own (jpdb: "来[き]た") or its dictionary
 * form's (jiten: "来[く]る"). Each kanji of the surface takes the reading
 * of the same kanji in the ruby; a dictionary-form reading that changes
 * with conjugation (来 く -> き/こ, 良 い -> よ) follows the kana after it;
 * kanji the ruby has no reading for are left without one. NULL on OOM. */
char *vjo_furigana_align(VjoArena *a, const char *surface, size_t n, const char *ruby);

#endif
