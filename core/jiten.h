/* jiten.moe /api/reader/parse client (request builder + response parser). */
#ifndef VJO_JITEN_H
#define VJO_JITEN_H

#include <stddef.h>

#include "arena.h"
#include "dict.h"

#define VJO_JITEN_HOST "api.jiten.moe"
#define VJO_JITEN_PATH "/api/reader/parse"

/* {"text":["<text>"]}: one paragraph, so token offsets index `text`. */
char *vjo_jiten_build_request(VjoArena *a, const char *text);

/* Returns 0 on success, -1 on malformed JSON or missing "vocabulary".
 * Repeated (wordId, readingIndex) pairs become one vocab entry. */
int vjo_jiten_parse_response(VjoArena *a, const char *json, size_t len, VjoDictResult *out);

/* "天[てん]気[き]" -> "てんき": each bracket replaces the kanji run before it. */
char *vjo_jiten_ruby_to_kana(VjoArena *a, const char *ruby);

/* Extracts "detail", "title", "message" or "error" from an error body. */
const char *vjo_jiten_error_message(VjoArena *a, const char *json, size_t len);

#endif
