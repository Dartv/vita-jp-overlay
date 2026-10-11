/* Overlay content model: header text + dictionary entries, one per
 * occurrence of a word (a word used twice is navigated to twice), with its
 * byte range in the header. */
#ifndef VJO_ENTRIES_H
#define VJO_ENTRIES_H

#include "arena.h"
#include "dict.h"
#include "furigana.h"

typedef struct {
    const VjoVocab *vocab;
    int pos16;              /* token position in the lookup text, -1 if none */
    int hl_start, hl_end;   /* byte range in VjoEntryList.header, -1 if none */
    const char *text;       /* "spelling (reading) rank\nmeaning..." (shared by a word's entries) */
    const char *furigana;   /* the token's VjoToken.furigana (ruby markup), NULL if none */
} VjoEntry;

typedef struct {
    const char *header;     /* filtered OCR text, trimmed */
    VjoEntry *entries;
    int n_entries;
    const char *source;     /* VjoDictResult.source, NULL without a lookup */
} VjoEntryList;

/* `filtered` is the non-Japanese-filtered OCR text (before newline removal);
 * token positions refer to vjo_strip_newlines(filtered). Entries are in
 * text order: one per token with a word, then words without a token (in
 * the dictionary's order). */
int vjo_entries_build(VjoArena *a, const char *filtered, const VjoDictResult *r, VjoEntryList *out);

/* The readings of the header's kanji (byte ranges in the header, in text
 * order) from the entries the header's furigana takes: those with text in
 * the header, less one overlapping the one before. Up to cap; returns the
 * number written. */
int vjo_entries_rubies(const VjoEntryList *l, VjoRuby *out, int cap);

/* Is entry i one of those? */
int vjo_entry_in_text(const VjoEntryList *l, int i);

/* Entry format: spelling + " (reading)" if different + " rank",
 * then "\n" + each meaning. */
char *vjo_entry_format(VjoArena *a, const VjoVocab *v);

/* Body text: entries joined by a blank line. */
char *vjo_entries_body(VjoArena *a, const VjoEntryList *l);

#endif
