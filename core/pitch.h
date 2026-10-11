/* Pitch accent: downstep positions and Yomitan's categories for them. */
#ifndef VJO_PITCH_H
#define VJO_PITCH_H

#include <stddef.h>

/* A high/low pattern, the n bytes "LHLL" (jpdb: the word's morae, then a
 * particle's) -> the downstep mora (2); 0 = none (heiban). */
int vjo_pitch_from_pattern(const char *pattern, size_t n);

/* Morae of a kana reading: small ゃゅょぁぃぅぇぉゎ (and katakana) join
 * the kana before them. */
int vjo_morae(const char *reading);

/* Yomitan's category for a downstep: 0 heiban; a verb or i-adjective
 * (part-of-speech codes, as Yomitan's isNonNounVerbOrAdjective) otherwise
 * kifuku; 1 atamadaka; the reading's mora count odaka; else nakadaka. */
const char *vjo_pitch_category(const char *reading, int downstep, const char *const *pos, int n_pos);

#endif
