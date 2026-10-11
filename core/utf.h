#ifndef VJO_UTF_H
#define VJO_UTF_H

#include <stddef.h>
#include <stdint.h>

/* Decodes one UTF-8 code point at s[*i] (advancing *i). Invalid bytes decode
 * as U+FFFD and consume one byte. */
uint32_t vjo_utf8_next(const char *s, size_t len, size_t *i);
/* Encodes cp into out (>= 4 bytes); returns byte count. */
int vjo_utf8_encode(uint32_t cp, char *out);
/* UTF-16 code units needed for code point cp. */
static inline int vjo_utf16_units(uint32_t cp) { return cp >= 0x10000 ? 2 : 1; }

/* ASCII case-insensitive equality, and whether s starts with prefix. */
int vjo_ieq(const char *a, const char *b);
int vjo_ieq_prefix(const char *s, const char *prefix);
/* Whether the n bytes at s start with prefix (exact bytes). */
int vjo_starts_withn(const char *s, size_t n, const char *prefix);

/* Cuts a UTF-8 sequence left incomplete at the end of s (by a truncating
 * snprintf), in place. */
void vjo_utf8_trim_partial(char *s);

/* Java String.trim(): strips chars <= U+0020 from both ends, in place;
 * returns the (possibly advanced) start pointer. */
char *vjo_java_trim(char *s);

/* Kanji, as furigana bases: CJK ideographs, 々〆 and the counters ヵヶ. */
static inline int vjo_is_kanji(uint32_t cp)
{
    return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x20000 && cp <= 0x3FFFF) || cp == 0x3005 /* 々 */ || cp == 0x3006 /* 〆 */ ||
           cp == 0x30F5 || cp == 0x30F6 /* ヵヶ */;
}

/* Japanese characters: [぀-ゟ゠-ヿ一-龯] */
static inline int vjo_is_japanese_char(uint32_t cp)
{
    return (cp >= 0x3040 && cp <= 0x309F) || (cp >= 0x30A0 && cp <= 0x30FF) ||
           (cp >= 0x4E00 && cp <= 0x9FAF);
}

#endif
