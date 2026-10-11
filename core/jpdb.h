/* jpdb.io /api/v1/parse client (request builder + response parser). */
#ifndef VJO_JPDB_H
#define VJO_JPDB_H

#include <stddef.h>

#include "arena.h"
#include "dict.h"

#define VJO_JPDB_HOST "jpdb.io"
#define VJO_JPDB_PATH "/api/v1/parse"

/* JSON body for the parse request. */
char *vjo_jpdb_build_request(VjoArena *a, const char *text);

/* Returns 0 on success, -1 on malformed JSON or missing "vocabulary". */
int vjo_jpdb_parse_response(VjoArena *a, const char *json, size_t len, VjoDictResult *out);

/* pitch_accent "LHLL" (the word's morae, then a particle's) -> the
 * downstep mora (2), 0 = none (heiban). */
int vjo_jpdb_pitch_position(const char *pattern);

/* Extracts "error_message" (or "error") from an error body; NULL if none. */
const char *vjo_jpdb_error_message(VjoArena *a, const char *json, size_t len);

#endif
