/* Overlay content model: header text + dictionary entries, with the header
 * byte range of each entry's first token. */
#ifndef VJO_ENTRIES_H
#define VJO_ENTRIES_H

#include "arena.h"
#include "dict.h"

typedef struct {
    const VjoVocab *vocab;
    int first_pos16;        /* first token position in the lookup text, -1 if none */
    int hl_start, hl_end;   /* byte range in VjoEntryList.header, -1 if none */
    const char *text;       /* "spelling (reading) rank\nmeaning..." */
} VjoEntry;

typedef struct {
    const char *header;     /* filtered OCR text, trimmed */
    VjoEntry *entries;
    int n_entries;
} VjoEntryList;

/* `filtered` is the non-Japanese-filtered OCR text (before newline removal);
 * token positions refer to vjo_strip_newlines(filtered). Entries are in
 * text order. */
int vjo_entries_build(VjoArena *a, const char *filtered, const VjoDictResult *r, VjoEntryList *out);

/* Entry format: spelling + " (reading)" if different + " rank",
 * then "\n" + each meaning. */
char *vjo_entry_format(VjoArena *a, const VjoVocab *v);

/* Body text: entries joined by a blank line. */
char *vjo_entries_body(VjoArena *a, const VjoEntryList *l);

#endif
