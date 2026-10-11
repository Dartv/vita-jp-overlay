/* Furigana in Anki's syntax: a space, the base, its reading in brackets
 * (" 降[ふ]っていた"), as Anki's furigana filter reads them. */
#ifndef VJO_FURIGANA_H
#define VJO_FURIGANA_H

#include <stddef.h>

#include "arena.h"

/* The n bytes of surface (a word as written: "来た") with readings taken
 * from its dictionary form's ruby (jiten: "来[く]る"). Each kanji run of
 * the surface takes the reading of the same kanji in the ruby; 来 read く
 * becomes き or こ by the kana after it; a run the ruby has no reading
 * for is left without one. NULL on OOM. */
char *vjo_furigana_align(VjoArena *a, const char *surface, size_t n, const char *ruby);

#endif
