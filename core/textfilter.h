/* OCR text post-processing. */
#ifndef VJO_TEXTFILTER_H
#define VJO_TEXTFILTER_H

#include "arena.h"

enum { VJO_FILTER_NONE = 0, VJO_FILTER_LINES = 1 };

/* non_japanese_filter = lines: split on \r?\n, keep lines containing a
 * Japanese char, join with \n. NONE returns a copy. */
char *vjo_filter_lines(VjoArena *a, const char *text, int mode);

/* Removes "\r\n" and "\n" from the text. */
char *vjo_strip_newlines(VjoArena *a, const char *text);

#endif
